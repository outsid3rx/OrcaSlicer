"""Verify discovery-file privacy using the already open file, not a second path lookup."""

import os


def private_file(fd: int, info) -> bool:
    if os.name == "posix":
        return info.st_uid == os.geteuid() and not info.st_mode & 0o077
    if os.name != "nt":
        return False
    # Match the native server's protected, current-user-only ACL. pywin32 is an
    # explicit Windows-only dependency; never silently skip a failed ACL check.
    import msvcrt
    import win32api
    import win32con
    import win32security
    import pywintypes

    try:
        security = win32security.GetSecurityInfo(
            msvcrt.get_osfhandle(fd), win32security.SE_FILE_OBJECT,
            win32security.OWNER_SECURITY_INFORMATION | win32security.DACL_SECURITY_INFORMATION)
        token = win32security.OpenProcessToken(win32api.GetCurrentProcess(), win32con.TOKEN_QUERY)
        try:
            sid = win32security.GetTokenInformation(token, win32security.TokenUser)[0]
        finally:
            token.Close()
        if security.GetSecurityDescriptorOwner() != sid:
            return False
        control, _ = security.GetSecurityDescriptorControl()
        acl = security.GetSecurityDescriptorDacl()
        if not control & win32security.SE_DACL_PROTECTED or acl is None or acl.GetAceCount() != 1:
            return False
        header, _mask, principal = acl.GetAce(0)
        return header[0] == win32security.ACCESS_ALLOWED_ACE_TYPE and principal == sid and not header[1] & win32con.INHERITED_ACE
    except (OSError, pywintypes.error):
        return False
