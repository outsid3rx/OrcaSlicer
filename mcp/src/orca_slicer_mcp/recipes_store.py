"""Private SQLite recipe revisions and durable, single-dispatch application intents.

Native calls are deliberately absent: callers finish preflight before claiming an
application and persist its receipt before observing the next native job state.
"""
from contextlib import contextmanager
import copy
import errno
import json
import os
from pathlib import Path
import sqlite3
import stat
import threading
import uuid

from .permissions import private_file
from .recipes_contract import (RecipeError, canonical, digest, validate, validate_request,
                               validate_record, validate_dump, parse_dump, revision_ref,
                               utc_now, expires_at, expired, warning)

STORAGE_VERSION = 1
LIBRARY_TOOL = "orca_recipe_library"
APPLY_TOOL = "orca_recipe_apply"


def default_database() -> Path:
    import sys
    if sys.platform == "darwin":
        base = Path.home() / "Library/Application Support"
    elif os.name == "nt":
        base = Path(os.environ.get("LOCALAPPDATA", Path.home() / "AppData/Local"))
    else:
        base = Path(os.environ.get("XDG_DATA_HOME", Path.home() / ".local/share"))
    return base / "orca-slicer-mcp/recipes.sqlite3"


def _fail(kind, message, details=None):
    raise RecipeError(kind, message, details or {})


def _private_directory(path):
    created = not path.exists()
    if created:
        path.mkdir(mode=0o700, parents=True)
    info = path.lstat()
    if stat.S_ISLNK(info.st_mode) or not stat.S_ISDIR(info.st_mode):
        _fail("library_conflict", "Recipe storage directory must be a real private directory")
    if os.name == "posix":
        if info.st_uid != os.geteuid() or info.st_mode & 0o077:
            _fail("library_conflict", "Recipe storage directory must be current-user-only")
    elif os.name == "nt":
        if created:
            _protect_windows(path, directory=True)
        if not _windows_private_path(path):
            _fail("library_conflict", "Recipe storage directory must have a protected current-user ACL")
    else:
        _fail("library_conflict", "Recipe storage privacy is unavailable on this platform")


def _protect_windows(path, directory=False):
    # Windows ownership and protected ACLs are required rather than emulated by
    # chmod. This mirrors descriptor privacy in permissions.py.
    import win32api
    import win32con
    import win32security
    token = win32security.OpenProcessToken(win32api.GetCurrentProcess(), win32con.TOKEN_QUERY)
    try:
        sid = win32security.GetTokenInformation(token, win32security.TokenUser)[0]
    finally:
        token.Close()
    acl = win32security.ACL()
    flags = win32con.OBJECT_INHERIT_ACE | win32con.CONTAINER_INHERIT_ACE if directory else 0
    acl.AddAccessAllowedAceEx(win32security.ACL_REVISION, flags, win32con.GENERIC_ALL, sid)
    win32security.SetNamedSecurityInfo(str(path), win32security.SE_FILE_OBJECT,
        win32security.DACL_SECURITY_INFORMATION | win32security.PROTECTED_DACL_SECURITY_INFORMATION,
        None, None, acl, None)


def _windows_private_path(path):
    import win32api
    import win32con
    import win32security
    token = win32security.OpenProcessToken(win32api.GetCurrentProcess(), win32con.TOKEN_QUERY)
    try:
        sid = win32security.GetTokenInformation(token, win32security.TokenUser)[0]
    finally:
        token.Close()
    security = win32security.GetNamedSecurityInfo(str(path), win32security.SE_FILE_OBJECT,
        win32security.OWNER_SECURITY_INFORMATION | win32security.DACL_SECURITY_INFORMATION)
    control, _ = security.GetSecurityDescriptorControl()
    acl = security.GetSecurityDescriptorDacl()
    if security.GetSecurityDescriptorOwner() != sid or not control & win32security.SE_DACL_PROTECTED or acl is None or acl.GetAceCount() != 1:
        return False
    header, _mask, principal = acl.GetAce(0)
    return header[0] == win32security.ACCESS_ALLOWED_ACE_TYPE and principal == sid and not header[1] & win32con.INHERITED_ACE


def _private_file(path, create=False):
    flags = os.O_RDWR | getattr(os, "O_NOFOLLOW", 0) | getattr(os, "O_NONBLOCK", 0) | getattr(os, "O_BINARY", 0)
    if create:
        flags |= os.O_CREAT | os.O_EXCL
    fd = os.open(path, flags, 0o600)
    try:
        info = os.fstat(fd)
        if not stat.S_ISREG(info.st_mode):
            _fail("library_conflict", "Recipe storage must be a regular file")
        if create and os.name == "nt":
            _protect_windows(path)
        if not private_file(fd, info):
            _fail("library_conflict", "Recipe storage must be owned and private to the current user")
        return fd
    except BaseException:
        os.close(fd)
        raise


class _OwnerLock:
    """A unique lifetime lock avoids PID reuse and cross-client startup guesses."""
    def __init__(self, directory):
        self.token = str(uuid.uuid4())
        self.path = directory / self.token
        self.fd = _private_file(self.path, create=True)
        try:
            os.write(self.fd, b"\0")
            self._acquire(self.fd)
        except BaseException:
            os.close(self.fd)
            raise

    @staticmethod
    def _acquire(fd):
        if os.name == "posix":
            import fcntl
            fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        else:
            import msvcrt
            os.lseek(fd, 0, os.SEEK_SET)
            msvcrt.locking(fd, msvcrt.LK_NBLCK, 1)

    @staticmethod
    def _release(fd):
        if os.name == "posix":
            import fcntl
            fcntl.flock(fd, fcntl.LOCK_UN)
        else:
            import msvcrt
            os.lseek(fd, 0, os.SEEK_SET)
            msvcrt.locking(fd, msvcrt.LK_UNLCK, 1)

    @classmethod
    def alive(cls, directory, token):
        try:
            if str(uuid.UUID(token)) != token:
                return None
            fd = _private_file(directory / token)
        except (OSError, RecipeError, ValueError):
            # Missing/corrupt owner evidence cannot establish a safe retry.
            return None
        try:
            try:
                cls._acquire(fd)
            except OSError as error:
                return True if error.errno in (errno.EACCES, errno.EAGAIN, errno.EDEADLK) else None
            cls._release(fd)
            return False
        finally:
            os.close(fd)

    def close(self):
        if self.fd is not None:
            self._release(self.fd)
            os.close(self.fd)
            self.fd = None
        # Retain the unique file: deleting lock inodes introduces probe races.


_SCHEMA = [
    "CREATE TABLE meta (key TEXT PRIMARY KEY, value TEXT NOT NULL)",
    """CREATE TABLE revisions (
        recipe_id TEXT NOT NULL, revision INTEGER NOT NULL, sha256 TEXT NOT NULL,
        payload TEXT NOT NULL, search_text TEXT NOT NULL,
        PRIMARY KEY(recipe_id,revision))""",
    """CREATE TABLE heads (
        recipe_id TEXT PRIMARY KEY, revision INTEGER NOT NULL,
        head_version INTEGER NOT NULL CHECK(head_version>0),
        archived INTEGER NOT NULL CHECK(archived IN (0,1)),
        FOREIGN KEY(recipe_id,revision) REFERENCES revisions(recipe_id,revision))""",
    """CREATE TABLE requests (
        request_id TEXT PRIMARY KEY, tool TEXT NOT NULL, action TEXT NOT NULL,
        argument_hash TEXT NOT NULL, result TEXT, operation_id TEXT)""",
    """CREATE TABLE plans (
        plan_id TEXT PRIMARY KEY, sha256 TEXT NOT NULL,
        public TEXT NOT NULL, native_request TEXT NOT NULL)""",
    """CREATE TABLE import_plans (
        plan_id TEXT PRIMARY KEY, sha256 TEXT NOT NULL,
        public TEXT NOT NULL, dump TEXT NOT NULL)""",
    """CREATE TABLE operations (
        operation_id TEXT PRIMARY KEY, plan_id TEXT NOT NULL UNIQUE,
        public TEXT NOT NULL, version INTEGER NOT NULL, owner TEXT NOT NULL,
        FOREIGN KEY(plan_id) REFERENCES plans(plan_id))""",
]


class RecipeStore:
    def __init__(self, path):
        self.path = Path(path).expanduser()
        if not self.path.is_absolute():
            _fail("invalid_params", "Recipe database path must be absolute")
        self._mutex = threading.RLock()
        self.conn = None
        self._owner = None
        try:
            _private_directory(self.path.parent)
            if self.path.is_symlink():
                _fail("library_conflict", "Recipe database cannot be a symlink")
            try:
                fd = _private_file(self.path, create=True)
            except FileExistsError:
                fd = _private_file(self.path)
            os.close(fd)
            for suffix in ("-wal", "-shm", "-journal"):
                sidecar = Path(str(self.path) + suffix)
                if sidecar.exists() or sidecar.is_symlink():
                    os.close(_private_file(sidecar))
            self.conn = sqlite3.connect(str(self.path), isolation_level=None,
                                        timeout=5, check_same_thread=False)
            self.conn.row_factory = sqlite3.Row
            self.conn.execute("PRAGMA busy_timeout=5000")
            version = self.conn.execute("PRAGMA user_version").fetchone()[0]
            if version not in (0, STORAGE_VERSION):
                _fail("unsupported_format", "Recipe database storage version is unsupported; no writes were performed")
            self.conn.execute("PRAGMA foreign_keys=ON")
            self.conn.execute("PRAGMA trusted_schema=OFF")
            if version == 0:
                self.conn.execute("BEGIN IMMEDIATE")
                try:
                    if self.conn.execute("PRAGMA user_version").fetchone()[0] == 0:
                        tables = self.conn.execute("SELECT name FROM sqlite_master WHERE type='table'").fetchall()
                        if tables:
                            _fail("unsupported_format", "Unversioned nonempty database is not a recipe library")
                        for sql in _SCHEMA:
                            self.conn.execute(sql)
                        self.conn.executemany("INSERT INTO meta VALUES (?,?)", [
                            ("library_id", str(uuid.uuid4())), ("generation", "0")])
                        self.conn.execute("PRAGMA user_version=1")
                    self.conn.execute("COMMIT")
                except BaseException:
                    self.conn.execute("ROLLBACK")
                    raise
            self.conn.execute("PRAGMA journal_mode=WAL")
            self.conn.execute("PRAGMA synchronous=FULL")
            self.owners_directory = self.path.with_name(self.path.name + ".owners")
            _private_directory(self.owners_directory)
            self._owner = _OwnerLock(self.owners_directory)
            identity = self.conn.execute("SELECT value FROM meta WHERE key='library_id'").fetchone()
            if identity is None:
                _fail("library_conflict", "Recipe database has no library identity")
            self.library_id = identity[0]
            if str(uuid.UUID(self.library_id)) != self.library_id:
                _fail("library_conflict", "Recipe database has an invalid library identity")
        except BaseException as error:
            self.close()
            if isinstance(error, (OSError, sqlite3.DatabaseError, ImportError, ValueError)):
                raise RecipeError("library_conflict", "Recipe storage could not be opened privately and consistently") from error
            raise

    def close(self):
        with self._mutex:
            if self.conn is not None:
                self.conn.close()
                self.conn = None
            if self._owner is not None:
                self._owner.close()
                self._owner = None

    @contextmanager
    def _tx(self, write=False):
        with self._mutex:
            if self.conn is None:
                _fail("library_conflict", "Recipe library is closed")
            try:
                self.conn.execute("BEGIN IMMEDIATE" if write else "BEGIN")
                if self.conn.execute("PRAGMA user_version").fetchone()[0] != STORAGE_VERSION:
                    _fail("unsupported_format", "Storage schema changed; restart with a compatible MCP server")
                yield self.conn
                self.conn.execute("COMMIT")
            except BaseException as error:
                if self.conn.in_transaction:
                    self.conn.execute("ROLLBACK")
                if isinstance(error, sqlite3.OperationalError) and any(s in str(error).lower() for s in ("locked", "busy")):
                    raise RecipeError("library_busy", "Another client is updating the recipe library") from None
                if isinstance(error, sqlite3.DatabaseError):
                    raise RecipeError("library_conflict", "Recipe database is inconsistent or unavailable") from error
                raise

    @staticmethod
    def _json(value):
        return canonical(value).decode("utf-8")

    @staticmethod
    def _hash_args(tool, action, parameters):
        return digest({"tool": tool, "action": action, "parameters": parameters})

    def _replay(self, db, request_id, tool, action, parameters):
        row = db.execute("SELECT * FROM requests WHERE request_id=?", (request_id,)).fetchone()
        if row is None:
            return None
        if (row["tool"], row["action"], row["argument_hash"]) != (tool, action, self._hash_args(tool, action, parameters)):
            _fail("request_conflict", "Request ID is already bound to different arguments")
        if row["operation_id"]:
            return self._operation(db, row["operation_id"])[0]
        result = json.loads(row["result"])
        if isinstance(result, dict) and set(result) == {"__recipe_failure__"}:
            error = result["__recipe_failure__"]
            raise RecipeError(error["kind"], error["message"], error.get("details", {}))
        return result

    def fail_request(self, request_id, tool, action, parameters, error):
        """Bind a validated failed request without replacing receipts or intents."""
        try:
            validate_request(tool, action, parameters, request_id)
        except RecipeError:
            return
        with self._tx(write=True) as db:
            if db.execute("SELECT 1 FROM requests WHERE request_id=?", (request_id,)).fetchone():
                return
            self._receipt(db, request_id, tool, action, parameters,
                          {"__recipe_failure__": error.as_dict()})

    def replay(self, request_id, tool, action, parameters):
        with self._tx(write=True) as db:
            return self._replay(db, request_id, tool, action, parameters)

    def _receipt(self, db, request_id, tool, action, parameters, result=None, operation_id=None):
        db.execute("INSERT INTO requests VALUES (?,?,?,?,?,?)", (
            request_id, tool, action, self._hash_args(tool, action, parameters),
            self._json(result) if result is not None else None, operation_id))

    def _mutate(self, request_id, action, parameters, fn, tool=LIBRARY_TOOL):
        validate_request(tool, action, parameters, request_id)
        try:
            with self._tx(write=True) as db:
                previous = self._replay(db, request_id, tool, action, parameters)
                if previous is not None:
                    return previous
                result = fn(db)
                self._receipt(db, request_id, tool, action, parameters, result)
                return result
        except RecipeError as error:
            self.fail_request(request_id, tool, action, parameters, error)
            raise

    @staticmethod
    def _generation(db):
        return int(db.execute("SELECT value FROM meta WHERE key='generation'").fetchone()[0])

    def _advance_generation(self, db):
        value = self._generation(db) + 1
        db.execute("UPDATE meta SET value=? WHERE key='generation'", (str(value),))
        return value

    def describe(self):
        with self._tx() as db:
            return {"library_id": self.library_id, "library_generation": self._generation(db),
                    "storage_schema_version": STORAGE_VERSION, "format_version": 1, "contract_version": 1,
                    "limits": {"dump_bytes": 8 * 1024 * 1024, "recipe_bytes": 65536,
                               "plan_recipes": 16, "native_keys": 256, "native_request_bytes": 65536},
                    "migrations": [], "actions": {
                        "orca_recipes": ["describe", "list", "get", "check", "export", "operation.get"],
                        LIBRARY_TOOL: ["create", "revise", "fork", "archive", "import.prepare", "import.commit"],
                        APPLY_TOOL: ["plan", "apply"]}}

    @staticmethod
    def _head(db, recipe_id):
        row = db.execute("""SELECT h.*,r.sha256 FROM heads h JOIN revisions r
            ON r.recipe_id=h.recipe_id AND r.revision=h.revision WHERE h.recipe_id=?""", (recipe_id,)).fetchone()
        if row is None:
            _fail("not_found", "Recipe ID was not found")
        return {"ref": {"recipe_id": recipe_id, "revision": row["revision"], "sha256": row["sha256"]},
                "head_version": row["head_version"], "archived": bool(row["archived"])}

    def get_head(self, recipe_id):
        with self._tx() as db:
            return self._head(db, recipe_id)

    @staticmethod
    def _record(db, reference):
        validate("RevisionRef", reference)
        row = db.execute("SELECT payload,sha256 FROM revisions WHERE recipe_id=? AND revision=?",
                         (reference["recipe_id"], reference["revision"])).fetchone()
        if row is None:
            _fail("not_found", "Exact recipe revision was not found")
        if row["sha256"] != reference["sha256"]:
            _fail("identity_conflict", "Recipe reference has a different content digest")
        record = {"payload": json.loads(row["payload"]), "sha256": row["sha256"]}
        validate_record(record)
        return record

    def get(self, reference):
        with self._tx() as db:
            record = self._record(db, reference)
            head = self._head(db, reference["recipe_id"])
            source = record["payload"].get("derived_from")
            incomplete = False
            if source:
                row = db.execute("SELECT sha256 FROM revisions WHERE recipe_id=? AND revision=?", (source["recipe_id"], source["revision"])).fetchone()
                incomplete = row is None or row[0] != source["sha256"]
            return {"record": record, "head": head["ref"], "head_version": head["head_version"],
                    "archived": head["archived"], "lineage_incomplete": incomplete}

    def list_recipes(self, query="", include_archived=False, offset=0, limit=100):
        validate_request("orca_recipes", "list", {"query": query, "include_archived": include_archived,
                                                  "offset": offset, "limit": limit})
        with self._tx() as db:
            rows = db.execute("""SELECT h.*,r.payload,r.sha256 FROM heads h JOIN revisions r
                ON r.recipe_id=h.recipe_id AND r.revision=h.revision
                WHERE (? OR h.archived=0) AND instr(r.search_text,?)>0
                ORDER BY h.recipe_id LIMIT ? OFFSET ?""", (include_archived, query.casefold(), limit + 1, offset)).fetchall()
            items = []
            for row in rows[:limit]:
                p = json.loads(row["payload"])
                items.append({"ref": {"recipe_id": row["recipe_id"], "revision": row["revision"], "sha256": row["sha256"]},
                    "name": p["name"], "scope": p["scope"], "tags": p.get("tags", []),
                    "deprecated": p.get("deprecated", False), "head_version": row["head_version"], "archived": bool(row["archived"])})
            return {"items": items, "offset": offset, "limit": limit,
                    "next_offset": offset + limit if len(rows) > limit else None}

    @staticmethod
    def _insert_record(db, record):
        p = record["payload"]
        search = "\n".join([p["name"], p["description"], *p.get("tags", [])]).casefold()
        db.execute("INSERT INTO revisions VALUES (?,?,?,?,?)", (p["recipe_id"], p["revision"],
            record["sha256"], canonical(p).decode("utf-8"), search))

    def _new_record(self, draft, native_contract, recipe_id=None, revision=1, parent=None, source=None):
        payload = copy.deepcopy(draft)
        payload.setdefault("evidence", [])
        payload.update(format="orca.recipe", format_version=1, recipe_id=recipe_id or str(uuid.uuid4()),
                       revision=revision, created_at=utc_now(), parent=parent, derived_from=source,
                       native_contract=copy.deepcopy(native_contract))
        record = {"payload": payload, "sha256": digest(payload)}
        validate_record(record)
        return record

    def _created_result(self, db, record):
        head = self._head(db, record["payload"]["recipe_id"])
        return {"record": record, "head": head["ref"], "head_version": head["head_version"],
                "archived": head["archived"], "library_generation": self._generation(db)}

    def create(self, request_id, parameters, native_contract):
        def run(db):
            if parameters["draft"]["scope"] != parameters["capture_binding"]["scope"]:
                _fail("invalid_params", "Draft and capture scope differ")
            record = self._new_record(parameters["draft"], native_contract)
            self._insert_record(db, record)
            p = record["payload"]
            db.execute("INSERT INTO heads VALUES (?,?,1,0)", (p["recipe_id"], 1))
            self._advance_generation(db)
            return self._created_result(db, record)
        return self._mutate(request_id, "create", parameters, run)

    def revise(self, request_id, parameters, native_contract):
        def run(db):
            head = self._head(db, parameters["recipe_id"])
            if head["head_version"] != parameters["expected_head_version"]:
                _fail("library_conflict", "Recipe head changed; read it before revising")
            maximum = db.execute("SELECT max(revision) FROM revisions WHERE recipe_id=?", (parameters["recipe_id"],)).fetchone()[0]
            if head["ref"]["revision"] != maximum:
                _fail("head_not_latest", "Advance the head explicitly or fork before revising")
            if head["archived"]:
                _fail("library_conflict", "Unarchive the recipe before revising")
            if parameters["draft"]["scope"] != parameters["capture_binding"]["scope"]:
                _fail("invalid_params", "Draft and capture scope differ")
            old = self._record(db, head["ref"])
            record = self._new_record(parameters["draft"], native_contract,
                recipe_id=parameters["recipe_id"], revision=maximum + 1, parent=head["ref"], source=old["payload"].get("derived_from"))
            self._insert_record(db, record)
            db.execute("UPDATE heads SET revision=?,head_version=head_version+1 WHERE recipe_id=?", (maximum + 1, parameters["recipe_id"]))
            self._advance_generation(db)
            return self._created_result(db, record)
        return self._mutate(request_id, "revise", parameters, run)

    def fork(self, request_id, parameters, native_contract):
        def run(db):
            self._record(db, parameters["source"])
            if parameters["draft"]["scope"] != parameters["capture_binding"]["scope"]:
                _fail("invalid_params", "Draft and capture scope differ")
            record = self._new_record(parameters["draft"], native_contract, source=parameters["source"])
            self._insert_record(db, record)
            db.execute("INSERT INTO heads VALUES (?,?,1,0)", (record["payload"]["recipe_id"], 1))
            self._advance_generation(db)
            return self._created_result(db, record)
        return self._mutate(request_id, "fork", parameters, run)

    def archive(self, request_id, parameters):
        def run(db):
            head = self._head(db, parameters["recipe_id"])
            if head["head_version"] != parameters["expected_head_version"]:
                _fail("library_conflict", "Recipe head changed before archive update")
            if head["archived"] != parameters["archived"]:
                db.execute("UPDATE heads SET archived=?,head_version=head_version+1 WHERE recipe_id=?",
                           (parameters["archived"], parameters["recipe_id"]))
                self._advance_generation(db)
            head = self._head(db, parameters["recipe_id"])
            return {"head": head["ref"], "head_version": head["head_version"], "archived": head["archived"],
                    "library_generation": self._generation(db)}
        return self._mutate(request_id, "archive", parameters, run)

    def export(self, selection="all", include_archived=None):
        params = {"selection": selection}
        if include_archived is not None:
            params["include_archived"] = include_archived
        validate_request("orca_recipes", "export", params)
        with self._tx() as db:
            if selection == "all":
                rows = db.execute("SELECT recipe_id,archived FROM heads WHERE (? OR archived=0) ORDER BY recipe_id",
                                  (include_archived is not False,)).fetchall()
                identities = {row["recipe_id"] for row in rows}
                records = []
                for identifier in sorted(identities):
                    for row in db.execute("SELECT * FROM revisions WHERE recipe_id=? ORDER BY revision", (identifier,)):
                        records.append({"payload": json.loads(row["payload"]), "sha256": row["sha256"]})
                heads = [{"ref": self._head(db, identifier)["ref"], "archived": self._head(db, identifier)["archived"]} for identifier in sorted(identities)]
            else:
                chosen = {}
                for reference in selection:
                    record = self._record(db, reference)
                    while True:
                        ref = revision_ref(record)
                        chosen[(ref["recipe_id"], ref["revision"])] = record
                        parent = record["payload"]["parent"]
                        if parent is None:
                            break
                        record = self._record(db, parent)
                records = [chosen[k] for k in sorted(chosen)]
                heads = []
                for identifier in sorted({key[0] for key in chosen}):
                    highest = max(key[1] for key in chosen if key[0] == identifier)
                    heads.append({"ref": revision_ref(chosen[(identifier, highest)]), "archived": self._head(db, identifier)["archived"]})
            value = {"format": "orca.recipe-library", "format_version": 1, "exported_at": utc_now(),
                     "recipes": records, "heads": heads}
            validate_dump(value)
            return value

    def _import_effects(self, db, dump, advance_heads):
        incoming = {(r["payload"]["recipe_id"], r["payload"]["revision"]): r for r in dump["recipes"]}
        insert, duplicate = [], []
        for identity, record in incoming.items():
            row = db.execute("SELECT sha256 FROM revisions WHERE recipe_id=? AND revision=?", identity).fetchone()
            if row and row[0] != record["sha256"]:
                _fail("identity_conflict", "Imported recipe identity has different content", {"recipe_id": identity[0], "revision": identity[1]})
            (duplicate if row else insert).append(revision_ref(record))
        changes = []
        for head in dump["heads"]:
            if db.execute("SELECT 1 FROM heads WHERE recipe_id=?", (head["ref"]["recipe_id"],)).fetchone() is None:
                changes.append(head)
        seen = set()
        for reference in advance_heads:
            identifier = reference["recipe_id"]
            if identifier in seen:
                _fail("invalid_params", "advance_heads repeats a recipe ID")
            seen.add(identifier)
            old = self._head(db, identifier)
            if old["archived"]:
                _fail("library_conflict", "Unarchive before advancing its head")
            if reference["revision"] <= old["ref"]["revision"]:
                _fail("invalid_params", "advance_heads must move forward")
            current = reference
            while current["revision"] > old["ref"]["revision"]:
                record = incoming.get((identifier, current["revision"]))
                if record is None:
                    record = self._record(db, current)
                if record["sha256"] != current["sha256"]:
                    _fail("identity_conflict", "advance_heads digest does not match the imported chain")
                current = record["payload"]["parent"]
                if current is None:
                    _fail("invalid_dump", "Head advancement has no complete parent chain")
            if current != old["ref"]:
                _fail("identity_conflict", "Head advancement does not descend from the installed head")
            changes.append({"ref": reference, "archived": False})
        notes = []
        for record in dump["recipes"]:
            source = record["payload"].get("derived_from")
            if source:
                known = incoming.get((source["recipe_id"], source["revision"]))
                row = db.execute("SELECT sha256 FROM revisions WHERE recipe_id=? AND revision=?", (source["recipe_id"], source["revision"])).fetchone()
                if not ((known and known["sha256"] == source["sha256"]) or (row and row[0] == source["sha256"])):
                    notes.append(warning("lineage_incomplete", "Imported fork refers to an unavailable source revision"))
        return insert, duplicate, changes, list({w["id"]: w for w in notes}.values())

    def import_prepare(self, request_id, parameters):
        validate_request(LIBRARY_TOOL, "import.prepare", parameters, request_id)
        dump = parse_dump(parameters["dump"])
        def run(db):
            insert, duplicate, changes, warnings = self._import_effects(db, dump, parameters.get("advance_heads", []))
            plan = {"import_plan_id": str(uuid.uuid4()), "library_id": self.library_id,
                "base_library_generation": self._generation(db), "expires_at": expires_at(),
                "dump_sha256": digest(dump), "insert_refs": insert, "duplicate_refs": duplicate,
                "head_changes": changes, "warnings": warnings}
            plan["import_plan_sha256"] = digest(plan)
            validate("ImportPlan", plan)
            db.execute("INSERT INTO import_plans VALUES (?,?,?,?)", (plan["import_plan_id"], plan["import_plan_sha256"], self._json(plan), self._json(dump)))
            return plan
        return self._mutate(request_id, "import.prepare", parameters, run)

    @staticmethod
    def _ack(warnings, accepted):
        required = {w["id"] for w in warnings if w["acknowledgement_required"]}
        known = {w["id"] for w in warnings}
        if len(accepted) != len(set(accepted)) or not required.issubset(set(accepted)) or not set(accepted).issubset(known):
            _fail("invalid_params", "Acknowledge required warning IDs exactly; unknown IDs are rejected")

    def import_commit(self, request_id, parameters):
        def run(db):
            row = db.execute("SELECT * FROM import_plans WHERE plan_id=?", (parameters["import_plan_id"],)).fetchone()
            if row is None:
                _fail("not_found", "Import plan was not found")
            plan = json.loads(row["public"])
            if (plan["library_id"] != self.library_id or digest({k: v for k, v in plan.items() if k != "import_plan_sha256"}) != row["sha256"]):
                _fail("library_conflict", "Stored import plan is inconsistent")
            if parameters["import_plan_sha256"] != row["sha256"]:
                _fail("digest_mismatch", "Import plan digest does not match")
            if expired(plan["expires_at"]):
                _fail("plan_expired", "Import plan expired; prepare another explicitly")
            if plan["base_library_generation"] != self._generation(db):
                _fail("library_conflict", "Recipe library changed after import preparation")
            self._ack(plan["warnings"], parameters["accepted_warning_ids"])
            dump = json.loads(row["dump"])
            for record in dump["recipes"]:
                if db.execute("SELECT 1 FROM revisions WHERE recipe_id=? AND revision=?", (record["payload"]["recipe_id"], record["payload"]["revision"])).fetchone() is None:
                    self._insert_record(db, record)
            for change in plan["head_changes"]:
                ref = change["ref"]
                if db.execute("SELECT 1 FROM heads WHERE recipe_id=?", (ref["recipe_id"],)).fetchone():
                    db.execute("UPDATE heads SET revision=?,head_version=head_version+1 WHERE recipe_id=?", (ref["revision"], ref["recipe_id"]))
                else:
                    db.execute("INSERT INTO heads VALUES (?,?,1,?)", (ref["recipe_id"], ref["revision"], change["archived"]))
            if plan["insert_refs"] or plan["head_changes"]:
                self._advance_generation(db)
            return {"inserted": plan["insert_refs"], "duplicates": plan["duplicate_refs"],
                    "head_changes": plan["head_changes"], "library_generation": self._generation(db)}
        return self._mutate(request_id, "import.commit", parameters, run)

    def save_plan(self, request_id, parameters, plan, native_request):
        def run(db):
            validate("Plan", plan)
            if plan["library_id"] != self.library_id or digest({k: v for k, v in plan.items() if k != "plan_sha256"}) != plan["plan_sha256"]:
                _fail("digest_mismatch", "Application plan digest or library identity is invalid")
            for reference in plan["recipes"]:
                self._record(db, reference)
                if self._head(db, reference["recipe_id"])["archived"]:
                    _fail("stale_plan", "A recipe was archived before plan storage")
            expected_params = copy.deepcopy(plan["binding"])
            if "height_range" in expected_params:
                expected_params["range"] = expected_params.pop("height_range")
            expected_params["values"] = plan["values"]
            if (set(native_request) != {"jsonrpc", "id", "method", "params"} or
                native_request.get("jsonrpc") != "2.0" or native_request.get("params") != expected_params):
                _fail("invalid_params", "Frozen native request differs from the public plan")
            body = json.dumps(native_request, ensure_ascii=False, separators=(",", ":"), allow_nan=False)
            if (native_request.get("method") != "settings.patch" or native_request.get("id") != plan["native_request_id"] or
                len(body.encode()) != plan["native_request_bytes"] or len(body.encode()) > 65536):
                _fail("native_limit", "Frozen native patch is inconsistent or exceeds its wire limit")
            db.execute("INSERT INTO plans VALUES (?,?,?,?)", (plan["plan_id"], plan["plan_sha256"], self._json(plan), body))
            return plan
        return self._mutate(request_id, "plan", parameters, run, APPLY_TOOL)

    def _plan(self, db, plan_id, sha256=None):
        row = db.execute("SELECT * FROM plans WHERE plan_id=?", (plan_id,)).fetchone()
        if row is None:
            _fail("not_found", "Application plan was not found in this library")
        if sha256 is not None and sha256 != row["sha256"]:
            _fail("digest_mismatch", "Application plan digest does not match")
        public = json.loads(row["public"])
        if public["library_id"] != self.library_id or digest({k: v for k, v in public.items() if k != "plan_sha256"}) != row["sha256"]:
            _fail("library_conflict", "Stored application plan is inconsistent")
        return public, json.loads(row["native_request"])

    def get_plan(self, plan_id, plan_sha256=None):
        with self._tx() as db:
            return self._plan(db, plan_id, plan_sha256)

    def _operation(self, db, operation_id):
        row = db.execute("SELECT * FROM operations WHERE operation_id=?", (operation_id,)).fetchone()
        if row is None:
            _fail("not_found", "Application operation was not found")
        op = json.loads(row["public"])
        if op["status"] == "dispatching" and op["native_job_id"] is None:
            owner_alive = _OwnerLock.alive(self.owners_directory, row["owner"])
            if owner_alive is not True:
                op.update(status="outcome_unknown", verification="unavailable",
                    error={"kind": "outcome_unknown", "message": "Original dispatch owner is unavailable; native execution may have happened"},
                    recovery={"action": "inspect_scene_do_not_resend", "message": "Inspect the current scene and original intent; this plan will not be dispatched again"})
                db.execute("UPDATE operations SET public=?,version=version+1 WHERE operation_id=? AND version=?",
                           (self._json(op), operation_id, row["version"]))
                return op, row["version"] + 1
        return op, row["version"]

    def existing_apply(self, request_id, parameters):
        validate_request(APPLY_TOOL, "apply", parameters, request_id)
        with self._tx(write=True) as db:
            previous = self._replay(db, request_id, APPLY_TOOL, "apply", parameters)
            if previous is not None:
                return previous
            self._plan(db, parameters["plan_id"], parameters["plan_sha256"])
            row = db.execute("SELECT operation_id FROM operations WHERE plan_id=?", (parameters["plan_id"],)).fetchone()
            if row:
                op = self._operation(db, row[0])[0]
                self._receipt(db, request_id, APPLY_TOOL, "apply", parameters, operation_id=row[0])
                return op
            return None

    def claim_apply(self, request_id, parameters, initial_operation):
        validate_request(APPLY_TOOL, "apply", parameters, request_id)
        with self._tx(write=True) as db:
            previous = self._replay(db, request_id, APPLY_TOOL, "apply", parameters)
            if previous is not None:
                return previous, False
            plan, _ = self._plan(db, parameters["plan_id"], parameters["plan_sha256"])
            row = db.execute("SELECT operation_id FROM operations WHERE plan_id=?", (parameters["plan_id"],)).fetchone()
            if row:
                operation = self._operation(db, row[0])[0]
                is_new = False
            else:
                if expired(plan["expires_at"]):
                    _fail("plan_expired", "Application plan expired before its claim")
                for reference in plan["recipes"]:
                    if self._head(db, reference["recipe_id"])["archived"]:
                        _fail("stale_plan", "A recipe was archived before application")
                self._ack(plan["warnings"], parameters["accepted_warning_ids"])
                validate("Operation", initial_operation)
                if (initial_operation["plan_id"] != plan["plan_id"] or initial_operation["plan_sha256"] != plan["plan_sha256"] or
                    initial_operation["native_request_id"] != plan["native_request_id"]):
                    _fail("invalid_params", "Application intent does not match its frozen plan")
                operation = copy.deepcopy(initial_operation)
                db.execute("INSERT INTO operations VALUES (?,?,?,1,?)", (operation["operation_id"], plan["plan_id"], self._json(operation), self._owner.token))
                is_new = True
            self._receipt(db, request_id, APPLY_TOOL, "apply", parameters, operation_id=operation["operation_id"])
            return operation, is_new

    def get_operation(self, operation_id):
        with self._tx(write=True) as db:
            return self._operation(db, operation_id)[0]

    def operation_snapshot(self, operation_id):
        with self._tx(write=True) as db:
            op, version = self._operation(db, operation_id)
            plan, native = self._plan(db, op["plan_id"], op["plan_sha256"])
            return op, version, plan, native

    def update_operation(self, operation_id, expected_version, updates):
        with self._tx(write=True) as db:
            current, version = self._operation(db, operation_id)
            if version != expected_version:
                return current
            updated = {**current, **copy.deepcopy(updates)}
            for key in ("operation_id", "plan_id", "plan_sha256", "native_request_id"):
                if updated[key] != current[key]:
                    _fail("invalid_params", "Application identity cannot change")
            validate("Operation", updated)
            db.execute("UPDATE operations SET public=?,version=version+1 WHERE operation_id=? AND version=?",
                       (self._json(updated), operation_id, expected_version))
            return updated

    def backup(self):
        """Consistent explicit maintenance snapshot; never replaces the live DB."""
        directory = self.path.with_name(self.path.name + ".backups")
        _private_directory(directory)
        destination = directory / (str(uuid.uuid4()) + ".sqlite3")
        fd = _private_file(destination, create=True)
        os.close(fd)
        with self._mutex:
            if self.conn is None:
                _fail("library_conflict", "Recipe library is closed")
            if self.conn.execute("PRAGMA user_version").fetchone()[0] != STORAGE_VERSION:
                _fail("unsupported_format", "Cannot back up unsupported storage through this version")
            target = sqlite3.connect(str(destination))
            try:
                self.conn.backup(target)
            finally:
                target.close()
        return destination
