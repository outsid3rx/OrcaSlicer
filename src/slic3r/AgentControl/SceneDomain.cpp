#include "Domain.hpp"
#include "FileAccess.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/ObjectID.hpp"
#include "libslic3r/PrintBase.hpp"
#include "libslic3r/libslic3r.h"
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/Geometry.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/Format/STL.hpp"
#include "libslic3r/Format/3mf.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/Selection.hpp"
#include "slic3r/GUI/GUI_Geometry.hpp"
#include "slic3r/GUI/GUI_ObjectList.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/NotificationManager.hpp"
#include "slic3r/GUI/Jobs/ArrangeJob.hpp"
#include "slic3r/GUI/Jobs/Job.hpp"
#include "slic3r/GUI/Jobs/Worker.hpp"
#include <boost/filesystem/path.hpp>
#include <wx/string.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <ios>
#include <functional>
#include <future>
#include <fstream>
#include <sstream>
#include <memory>
#include <set>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace Slic3r::AgentControl {
namespace {
namespace fs = std::filesystem;
Json vec(const Vec3d& v) { return Json::array({v.x(),v.y(),v.z()}); }
Json bounds(const BoundingBoxf3& b) { return b.defined ? Json{{"min",vec(b.min)},{"max",vec(b.max)}} : Json(nullptr); }
Json matrix(const Transform3d& m) {
    Json rows=Json::array();
    for(int r=0;r<4;++r) rows.push_back(Json::array({m(r,0),m(r,1),m(r,2),m(r,3)}));
    return rows;
}
std::string id(const ObjectBase& v) { return std::to_string(v.id().id); }
Json string_schema() { return {{"type","string"},{"minLength",1},{"maxLength",4096}}; }
Json vector_schema() { return {{"type","array"},{"minItems",3},{"maxItems",3},{"items",{{"type","number"}}}}; }
std::string text(const Json& p,const char* key) {
    ensure(p.contains(key)&&p[key].is_string(),"invalid_params","Expected a string parameter");
    auto v=p[key].get<std::string>();
    ensure(!v.empty()&&v.size()<=4096,"invalid_params","String is empty or too long");
    ensure(v.find('\0')==std::string::npos,"invalid_params","NUL is not allowed");
    return v;
}
Vec3d vector(const Json& p,const char* key) {
    ensure(p.contains(key)&&p[key].is_array()&&p[key].size()==3,"invalid_params","Expected three finite numbers");
    Vec3d v;
    for(int i=0;i<3;++i) { ensure(p[key][i].is_number(),"invalid_params","Expected finite numbers"); v[i]=p[key][i].get<double>(); ensure(std::isfinite(v[i])&&std::abs(v[i])<=1e6,"invalid_params","Number outside supported range"); }
    return v;
}
struct ObjectRef { ModelObject* object; std::size_t index; };
ObjectRef object_ref(GUI::Plater& p,const std::string& wanted) {
    for(std::size_t i=0;i<p.model().objects.size();++i) if(id(*p.model().objects[i])==wanted) return {p.model().objects[i],i};
    throw DomainError("not_found","Object ID does not exist in this document");
}
struct InstanceRef { ModelObject* object; ModelInstance* instance; std::size_t oi,ii; };
InstanceRef instance_ref(GUI::Plater& p,const std::string& wanted) {
    for(std::size_t i=0;i<p.model().objects.size();++i) {
        auto* o=p.model().objects[i];
        for(std::size_t j=0;j<o->instances.size();++j) if(id(*o->instances[j])==wanted) return {o,o->instances[j],i,j};
    }
    throw DomainError("not_found","Instance ID does not exist in this document");
}
struct VolumeRef { ModelObject* object; ModelVolume* volume; std::size_t oi,vi; };
VolumeRef volume_ref(GUI::Plater& p,const std::string& wanted) {
    for(std::size_t i=0;i<p.model().objects.size();++i) {
        auto* o=p.model().objects[i];
        for(std::size_t j=0;j<o->volumes.size();++j) if(id(*o->volumes[j])==wanted) return {o,o->volumes[j],i,j};
    }
    throw DomainError("not_found","Volume ID does not exist in this document");
}
// print_index is monotonically allocated until a project reset. The model ID
// disambiguates reset/Undo restoration; state_token guards every mutation too.
std::string plate_id(DomainContext& ctx,GUI::PartPlate& plate) {
    int print_index=-1; PrintBase* print=nullptr; GUI::GCodeResult* result=nullptr;
    plate.get_print(&print,&result,&print_index);
    return "plate:"+std::to_string(ctx.document_epoch)+":"+std::to_string(print_index);
}
int plate_ref(DomainContext& ctx,const std::string& wanted) {
    auto& p=ctx.plater;auto& list=p.get_partplate_list();
    for(int i=0;i<list.get_plate_count();++i) if(plate_id(ctx,*list.get_plate(i))==wanted) return i;
    throw DomainError("stale_target","Plate ID is stale or unknown");
}
void unlocked(GUI::Plater& p,const InstanceRef& r) {
    auto& list=p.get_partplate_list();
    for(std::size_t j=0;j<r.object->instances.size();++j) {
        int pi=list.find_instance_belongs(static_cast<int>(r.oi),static_cast<int>(j));
        ensure(pi<0||pi>=list.get_plate_count()||!list.get_plate(pi)->is_locked(),"locked","A linked instance is on a locked plate");
    }
}
void touch_object(GUI::Plater& p,ModelObject& o) {
    o.invalidate_bounding_box();p.get_partplate_list().invalidate_exclusion_volume_previews();p.schedule_background_process();
}
void refresh(GUI::Plater& p) {
    for(auto* plate:p.get_partplate_list().get_plate_list())plate->update_slice_result_valid_state(false);
    p.get_partplate_list().reload_all_objects();
    GUI::wxGetApp().obj_list()->reload_all_plates();
    p.update(); p.object_list_changed(); p.set_plater_dirty(true);
}
// Detached objects keep their volume material IDs; their temporary owner must
// also own the corresponding materials before native geometry code visits them.
void copy_materials(Model& destination,const Model& source) {
    for(const auto& material:source.materials)
        destination.add_material(material.first,*material.second);
}
Json result(DomainContext&) { return {{"refresh_scene",true}}; }
void select(GUI::Plater& p,const InstanceRef& r) {
    auto& selection=p.get_view3D_canvas3D()->get_selection();
    selection.remove_all(); selection.add_instance(static_cast<unsigned>(r.oi),static_cast<unsigned>(r.ii)); selection.setup_cache();
}
std::set<std::string> object_ids(GUI::Plater& p) {
    std::set<std::string> v;for(auto* o:p.model().objects)v.insert(id(*o));return v;
}
Json added(GUI::Plater& p,const std::set<std::string>& before) {
    Json v=Json::array();for(auto* o:p.model().objects)if(!before.count(id(*o)))v.push_back(id(*o));return v;
}
class OwnedArrange final : public GUI::ArrangeJob {
    DomainContext m_ctx;
    std::shared_ptr<std::atomic<bool>> m_cancel;
    std::vector<std::string> m_targets;
public:
    OwnedArrange(DomainContext ctx,std::shared_ptr<std::atomic<bool>> cancel,std::vector<std::string> targets):GUI::ArrangeJob(true),m_ctx(std::move(ctx)),m_cancel(std::move(cancel)),m_targets(std::move(targets)){}
    void process(Ctl& native) override {
        struct Control final : Ctl {
            Ctl& inner;
            OwnedArrange& owner;
            Control(Ctl& ctl,OwnedArrange& job):inner(ctl),owner(job){}
            bool was_canceled() const override {return inner.was_canceled()||owner.m_cancel->load();}
            void update_status(int status,const std::string& message) override {inner.update_status(status,message);}
            void clear_percent() override {inner.clear_percent();}
            void show_error_info(const std::string& msg,int code,const std::string& description,const std::string& extra) override {inner.show_error_info(msg,code,description,extra);}
            std::future<void> call_on_main_thread(std::function<void()> fn) override {
                return inner.call_on_main_thread([this,fn=std::move(fn)] {
                    ensure(!owner.m_ctx.lifetime.expired(),"cancelled","Window closed before preparation");
                    ensure(!was_canceled(),"cancelled","Arrangement cancelled before preparation");
                    ensure(owner.m_ctx.current_state()==owner.m_ctx.state_token,"conflict","Scene changed before arrangement preparation");
                    auto& p=owner.m_ctx.plater;auto& selection=p.get_view3D_canvas3D()->get_selection();selection.remove_all();
                    for(const auto& key:owner.m_targets){auto r=instance_ref(p,key);selection.add_instance(static_cast<unsigned>(r.oi),static_cast<unsigned>(r.ii),false);}
                    selection.setup_cache();
                    fn();
                });
            }
        } control(native,*this);
        GUI::ArrangeJob::process(control);
    }
    void finalize(bool canceled,std::exception_ptr& error) override {
        const bool failed=static_cast<bool>(error);error=nullptr;
        if(m_ctx.lifetime.expired())return;
        const auto cleanup=[&]{m_ctx.plater.m_arrange_running.store(false);m_ctx.plater.get_notification_manager()->close_notification_of_type(GUI::NotificationType::ArrangeOngoing);};
        cleanup();
        if(canceled||m_cancel->load()) { m_ctx.fail("cancelled","Arrangement cancelled before commit");return; }
        if(failed) { m_ctx.fail("arrange_failed","Native arrangement failed");return; }
        if(m_ctx.current_state()!=m_ctx.state_token) { m_ctx.fail("conflict","Scene changed during arrangement");return; }
        if(!has_arranged_items()){m_ctx.fail("not_arrangeable","Native arranger could not place any selected instance");return;}
        m_ctx.plater.take_snapshot("Agent arrange");
        GUI::ArrangeJob::finalize(false,error);
        if(error) { error=nullptr;m_ctx.fail("arrange_failed","Native arrangement commit failed");return; }
        m_ctx.invalidate(true); m_ctx.finish(result(m_ctx));
    }
};
}

std::string plate_identity(DomainContext& ctx,const GUI::PartPlate& plate) {
    return plate_id(ctx,const_cast<GUI::PartPlate&>(plate));
}
GUI::PartPlate& resolve_plate(DomainContext& ctx,const Json& params) {
    ensure(params.contains("plate_id"),"invalid_params","plate_id is required");
    return *ctx.plater.get_partplate_list().get_plate(plate_ref(ctx,text(params,"plate_id")));
}

Json scene_page(DomainContext& ctx,std::size_t offset,std::size_t limit) {
    auto& p=ctx.plater;auto& plates=p.get_partplate_list();
    Json objects=Json::array(),instances=Json::array(),volumes=Json::array(),table=Json::array();
    const auto page=[=](std::size_t index){return index>=offset&&index-offset<limit;};
    std::size_t instance_count=0,volume_count=0;
    for(int i=0;i<plates.get_plate_count();++i) {
        if(!page(static_cast<std::size_t>(i)))continue;
        auto* plate=plates.get_plate(i);
        table.push_back({{"id",plate_id(ctx,*plate)},{"index",i},{"name",plate->get_plate_name()},{"origin",vec(plate->get_origin())},{"bounds",bounds(plate->get_bounding_box())},{"locked",plate->is_locked()},{"slice_valid",plate->is_slice_result_valid()}});
    }
    for(std::size_t oi=0;oi<p.model().objects.size();++oi) {
        auto* o=p.model().objects[oi];Json ii=Json::array(),vv=Json::array();bool on_current=false;
        for(std::size_t j=0;j<o->instances.size();++j) {
            auto* inst=o->instances[j];int pi=plates.find_instance_belongs(static_cast<int>(oi),static_cast<int>(j));
            on_current|=pi==plates.get_curr_plate_index();
            if(ii.size()<128)ii.push_back(id(*inst));
            if(!page(instance_count++))continue;
            Json pid=pi>=0&&pi<plates.get_plate_count()?Json(plate_id(ctx,*plates.get_plate(pi))):Json(nullptr);
            instances.push_back({{"id",id(*inst)},{"object_id",id(*o)},{"plate_id",pid},{"matrix",matrix(inst->get_matrix())},{"offset",vec(inst->get_offset())},{"rotation_radians",vec(inst->get_rotation())},{"scale",vec(inst->get_scaling_factor())},{"mirror",vec(inst->get_mirror())},{"bounds",bounds(o->instance_bounding_box(j))},{"printable",inst->printable},{"placement_valid",inst->is_printable()}});
        }
        for(auto* v:o->volumes) {
            if(vv.size()<128)vv.push_back(id(*v));
            if(!page(volume_count++))continue;
            volumes.push_back({{"id",id(*v)},{"object_id",id(*o)},{"name",v->name},{"type",static_cast<int>(v->type())},{"matrix",matrix(v->get_matrix())},{"mesh_revision",mesh_revision(*v)},{"vertices",v->mesh().its.vertices.size()},{"triangles",v->mesh().its.indices.size()},{"bounds",bounds(v->mesh().bounding_box())},{"support_painted",v->is_fdm_support_painted()},{"seam_painted",v->is_seam_painted()},{"material_painted",v->is_mm_painted()}});
        }
        if(!page(oi))continue;
        const auto exact=o->bounding_box_exact();
        objects.push_back({{"object_id",id(*o)},{"size_mm",vec(exact.size())},{"on_current_plate",on_current},{"id",id(*o)},{"name",o->name},{"printable",o->printable},{"auto_drop",o->auto_drop},{"instance_ids",ii},{"volume_ids",vv},{"instance_count",o->instances.size()},{"volume_count",o->volumes.size()},{"child_ids_truncated",o->instances.size()>128||o->volumes.size()>128},{"bounds",bounds(exact)}});
    }
    const auto max_count=std::max({p.model().objects.size(),instance_count,volume_count,static_cast<std::size_t>(plates.get_plate_count())});
    return {{"target",ctx.target},{"document_epoch",ctx.document_epoch},{"document_id",id(p.model())},{"state_token",ctx.state_token},{"plates",table},{"objects",objects},{"instances",instances},{"volumes",volumes},{"plate_index",plates.get_curr_plate_index()},{"current_plate_id",plate_id(ctx,*plates.get_curr_plate())},{"counts",{{"objects",p.model().objects.size()},{"instances",instance_count},{"volumes",volume_count},{"plates",plates.get_plate_count()}}},{"offset",offset},{"limit",limit},{"has_more",offset<max_count&&max_count-offset>limit},{"plate_handle_lifetime","document_epoch; refresh after scene mutation or Undo"},{"coordinates",{{"distance","mm"},{"matrix_layout","row_major"},{"matrix_order","world = instance * volume * vertex"},{"linked_instances","FFF shares Z, XY tilt, scale and mirror; auto_drop belongs to object"}}}};
}
Json describe_scene(DomainContext& ctx) {return scene_page(ctx,0,128);}

CommandSpecs scene_commands() {
    CommandSpecs s;const auto str=string_schema(),v=vector_schema();
    auto add=[&](const char* name,bool mutation,Json props,std::vector<std::string> req){s.emplace(name,command_spec(mutation,std::move(props),std::move(req)));};
    add("scene.describe",false,{{"offset",{{"type","integer"},{"minimum",0},{"maximum",1000000}}},{"limit",{{"type","integer"},{"minimum",1},{"maximum",128}}}},{});
    add("model.import",true,{{"paths",{{"type","array"},{"minItems",1},{"maxItems",16},{"items",str}}},{"auto_drop",{{"type","boolean"}}},{"step_linear_deflection",{{"type","number"},{"minimum",0.01},{"maximum",10}}},{"step_angle_radians",{{"type","number"},{"minimum",0.05},{"maximum",1.5}}},{"step_split_compound",{{"type","boolean"}}}},{"paths"});
    add("project.open",true,{{"path",str},{"load_config",{{"type","boolean"}}}},{"path","load_config"});
    for(const auto* method:{"project.save","model.export","gcode.export"})add(method,true,{{"path",str},{"overwrite",{{"type","boolean"}}},{"object_id",str},{"plate_id",str}},{"path"});
    add("plates.create",true,{{"name",str}},{});
    for(const auto* method:{"plates.select","plates.duplicate","plates.delete"})add(method,true,{{"plate_id",str}},{"plate_id"});
    add("plates.rename",true,{{"plate_id",str},{"name",str}},{"plate_id","name"});
    add("plates.reorder",true,{{"plate_id",str},{"index",{{"type","integer"},{"minimum",0}}}},{"plate_id","index"});
    add("plates.lock",true,{{"plate_id",str},{"locked",{{"type","boolean"}}}},{"plate_id","locked"});
    for(const auto* method:{"instances.translate","instances.rotate","instances.scale"})add(method,true,{{"instance_id",str},{"value",v},{"relative",{{"type","boolean"}}},{"frame",{{"type","string"},{"enum",{"world","object","plate"}}}},{"auto_drop",{{"type","boolean"}}}},{"instance_id","value","relative","frame"});
    add("instances.mirror",true,{{"instance_id",str},{"axis",{{"type","string"},{"enum",{"x","y","z"}}}}},{"instance_id","axis"});
    add("instances.move_to_plate",true,{{"instance_id",str},{"plate_id",str},{"position",v}},{"instance_id","plate_id","position"});
    for(const auto* method:{"instances.center","instances.drop","instances.clone","instances.delete","objects.make_independent"})add(method,true,{{"instance_id",str}},{"instance_id"});
    add("layout.arrange",true,{{"instance_ids",{{"type","array"},{"minItems",1},{"maxItems",256},{"items",str}}}},{"instance_ids"});
    for(const auto* op:{"translate","rotate","scale"}) {
        s.emplace(std::string("parts.")+op,command_spec(true,{{"volume_id",str},{"instance_id",str},{"mesh_revision",str},{"value",v},{"relative",{{"type","boolean"}}},{"frame",{{"type","string"},{"enum",{"world","object"}}}}},{"volume_id","instance_id","mesh_revision","value","relative","frame"}));
    }
    add("parts.mirror",true,{{"volume_id",str},{"instance_id",str},{"mesh_revision",str},{"axis",{{"type","string"},{"enum",{"x","y","z"}}}}},{"volume_id","instance_id","mesh_revision","axis"});
    add("instances.orient_to_face",true,{{"instance_id",str},{"normal",v}},{"instance_id","normal"});
    add("instances.printable",true,{{"instance_id",str},{"printable",{{"type","boolean"}}}},{"instance_id","printable"});
    add("objects.clone",true,{{"object_id",str}},{"object_id"});
    add("modifiers.set_type",true,{{"volume_id",str},{"type",{{"type","string"},{"enum",{"negative","modifier","support_blocker","support_enforcer"}}}}},{"volume_id","type"});
    add("objects.rename",true,{{"object_id",str},{"name",str}},{"object_id","name"});
    add("objects.printable",true,{{"object_id",str},{"printable",{{"type","boolean"}}}},{"object_id","printable"});
    for(const auto* method:{"objects.delete","objects.split"})add(method,true,{{"object_id",str}},{"object_id"});
    add("parts.split",true,{{"volume_id",str},{"mesh_revision",str}},{"volume_id","mesh_revision"});
    add("objects.merge",true,{{"object_ids",{{"type","array"},{"minItems",2},{"maxItems",64},{"items",str}}}},{"object_ids"});
    add("modifiers.add",true,{{"object_id",str},{"type",{{"type","string"},{"enum",{"negative","modifier","support_blocker","support_enforcer"}}}},{"shape",{{"type","string"},{"enum",{"box","sphere","cylinder"}}}},{"size",v},{"position",v},{"name",str}},{"object_id","type","shape","size","position"});
    add("modifiers.delete",true,{{"volume_id",str}},{"volume_id"});
    s["instances.rotate"].schema["properties"]["relative"]["enum"]=Json::array({true});
    for(const auto* op:{"parts.translate","parts.rotate","parts.scale"})s[op].schema["properties"]["relative"]["enum"]=Json::array({true});
    s["model.import"].schema["description"]="Geometry-only import; never imports print or network settings. Native parsing currently runs on the GUI thread.";
    s["project.open"].schema["description"]="Load a full 3MF project with native unsaved-project/preset dialogs; no automatic discard.";
    s["project.save"].schema["description"]="Save a complete 3MF copy atomically; current project filename and dirty state are preserved.";
    s["model.export"].schema["description"]="Export binary STL or geometry-only 3MF, selected by output filename extension.";
    s["gcode.export"].schema["description"]="Copy an already valid plate toolpath; does not run post-processing scripts.";
    s["instances.rotate"].schema["description"]="Relative X/Y/Z degrees applied sequentially around native selection center; linked-instance tilt invariants apply.";
    s["instances.scale"].schema["description"]="Positive relative factors in world/object frame, or absolute scaling factors in object frame.";
    s["instances.orient_to_face"].schema["description"]="Orient an object-space outward face normal downwards, using native Place on Face.";
    s["plates.delete"].schema["description"]="Delete an empty unlocked plate; move or delete contents explicitly first.";
    s["objects.merge"].schema["description"]="Multipart grouping, without boolean union. Same-plate single-instance objects with equal object overrides and no height ranges.";
    return s;
}

Json execute_scene(DomainContext& ctx,const std::string& method,const Json& params) {
    auto& p=ctx.plater;auto& list=p.get_partplate_list();
    const auto specs=scene_commands();auto spec=specs.find(method);
    ensure(spec!=specs.end(),"method_not_found","Unknown scene command");
    validate_parameters(params,spec->second);
    if(method=="scene.describe")return scene_page(ctx,params.value("offset",std::size_t(0)),params.value("limit",std::size_t(128)));
    if(method=="model.import") {
        ensure(params["paths"].is_array()&&!params["paths"].empty()&&params["paths"].size()<=16,"invalid_params","Supply 1 to 16 input files");
        Model prepared;Json sources=Json::array();std::uintmax_t input_bytes=0;
        for(const auto& path:params["paths"]) {
            InputSnapshot snapshot(path);const auto input=snapshot.path();input_bytes+=snapshot.size();ensure(input_bytes<=256ULL*1024*1024,"limit_exceeded","Import batch exceeds 256 MiB");const auto& hash=snapshot.sha256();
            auto extension=input.extension().string();std::transform(extension.begin(),extension.end(),extension.begin(),[](unsigned char c){return static_cast<char>(std::tolower(c));});
            ensure(extension==".stl"||extension==".obj"||extension==".3mf"||extension==".amf"||extension==".step"||extension==".stp","unsupported_format","Supported imports: STL OBJ 3MF AMF STEP");
            auto parse_input=input;bool ignored_materials=false;
            if(extension==".obj") {
                // Geometry import does not authorize opening OBJ sidecar paths from mtllib.
                parse_input=input.parent_path()/"geometry.obj";
                std::ifstream source(input);std::ofstream cleaned(parse_input,std::ios::binary);
                ensure(source.good()&&cleaned.good(),"file_error","Cannot stage geometry-only OBJ");
                std::string line;
                while(std::getline(source,line)) {
                    if(line.size()>=3&&static_cast<unsigned char>(line[0])==0xef&&static_cast<unsigned char>(line[1])==0xbb&&static_cast<unsigned char>(line[2])==0xbf)line.erase(0,3);
                    std::istringstream tokens(line);std::string keyword;tokens>>keyword;
                    std::transform(keyword.begin(),keyword.end(),keyword.begin(),[](unsigned char c){return static_cast<char>(std::tolower(c));});
                    if(keyword=="mtllib"){ignored_materials=true;continue;}
                    cleaned<<line<<'\n';
                }
                ensure(source.eof()&&cleaned.good(),"file_error","Cannot stage geometry-only OBJ");
                cleaned.close();fs::permissions(parse_input,fs::perms::owner_read|fs::perms::owner_write);
            }
            Model parsed;
            try {
                const auto strategy=LoadStrategy::LoadModel|LoadStrategy::AddDefaultInstances|LoadStrategy::Silence;
                if(extension==".step"||extension==".stp")parsed=Model::read_from_step(parse_input.u8string(),strategy,nullptr,nullptr,nullptr,params.value("step_linear_deflection",0.1),params.value("step_angle_radians",0.5),params.value("step_split_compound",true));
                else parsed=Model::read_from_file(parse_input.u8string(),nullptr,nullptr,strategy);
            } catch(const std::exception& error){throw DomainError("import_failed",error.what());}
            ensure(!parsed.objects.empty(),"import_failed","Input contains no model objects");
            ensure(file_sha256(input)==hash,"conflict","Private input snapshot changed while being parsed");
            for(auto* o:parsed.objects) {
                // Geometry import must not introduce executable or print presets, including overrides.
                if(o->name==input.stem().string()||o->name=="geometry")o->name=snapshot.original().stem().string();
                o->input_file=snapshot.original().u8string();o->config.reset();o->layer_config_ranges.clear();o->layer_height_profile.clear();
                for(auto* v:o->volumes){v->config.reset();v->set_material_id("");v->mmu_segmentation_facets.reset();v->source.input_file=snapshot.original().u8string();}
                prepared.add_object(*o);
            }
            sources.push_back({{"sha256",hash},{"size",snapshot.size()},{"external_materials_ignored",ignored_materials}});
        }
        const auto bed=list.get_curr_plate()->get_bounding_box().size();
        for(const auto* o:prepared.objects)for(std::size_t i=0;i<o->instances.size();++i){auto size=o->instance_bounding_box(i).size();ensure(size.x()<=bed.x()*10&&size.y()<=bed.y()*10,"model_too_large","Model exceeds native import size limit; supply explicitly scaled geometry");}
        auto before=object_ids(p);p.take_snapshot("Agent import models");
        p.agent_import_objects(prepared.objects,params.value("auto_drop",true));refresh(p);
        auto out=result(ctx);out["created_object_ids"]=added(p,before);out["sources"]=sources;return out;
    }
    if(method=="project.open") {
        ensure(params.at("load_config").is_boolean()&&params["load_config"].get<bool>(),"invalid_params","Use model.import for geometry-only loading; project.open requires explicit load_config=true");
        InputSnapshot snapshot(params.at("path"));const auto input=snapshot.path();
        ensure(input.extension()==".3mf","unsupported_format","Projects must be 3MF files");
        auto hash=snapshot.sha256();
        Model validation=Model::read_from_file(input.u8string(),nullptr,nullptr,LoadStrategy::LoadModel|LoadStrategy::AddDefaultInstances|LoadStrategy::Silence);
        ensure(!validation.objects.empty(),"import_failed","Project has no model objects");
        ensure(file_sha256(input)==hash,"conflict","Project changed while being validated");
        // The native loader retains its unsaved-project/preset decisions. No automatic discard.
        auto before=ctx.current_state();
        p.load_project(wxString::FromUTF8(input.u8string()),"<loadall>",wxString::FromUTF8(snapshot.original().u8string()));
        ensure(ctx.current_state()!=before,"cancelled","Project opening was cancelled or did not replace the document");
        for(auto* o:p.model().objects){if(o->input_file==input.u8string())o->input_file=snapshot.original().u8string();for(auto* v:o->volumes)if(v->source.input_file==input.u8string())v->source.input_file=snapshot.original().u8string();}
        auto out=result(ctx);out["source_sha256"]=hash;out["native_dialog_policy"]="user_decision";return out;
    }
    if(method=="project.save"||method=="model.export"||method=="gcode.export") {
        OutputArtifact file(params.at("path"),params.value("overwrite",false));const auto suffix=file.destination().extension().string();
        if(method=="project.save") {
            ensure(suffix==".3mf","unsupported_format","Project destination must end in .3mf");
            ensure(!params.contains("object_id")&&!params.contains("plate_id"),"invalid_params","Project save writes the complete project");
            ensure(p.export_3mf_copy(boost::filesystem::path(file.path().native())),"export_failed","Native project export failed");
        } else if(method=="model.export") {
            ensure(suffix==".stl"||suffix==".3mf","unsupported_format","Model export writes binary STL or geometry-only 3MF");
            ensure(!params.contains("plate_id"),"invalid_params","Use an object_id or export the whole model");
            Model copy=p.model();
            if(params.contains("object_id")) {
                auto wanted=text(params,"object_id");object_ref(p,wanted);
                for(std::size_t i=copy.objects.size();i>0;--i)if(id(*copy.objects[i-1])!=wanted)copy.delete_object(i-1);
            }
            ensure(!copy.objects.empty(),"empty_scene","There are no objects to export");
            if(suffix==".stl")ensure(store_stl(file.path().string().c_str(),&copy,true),"export_failed","Native STL export failed");
            else {
                for(auto* o:copy.objects){o->config.reset();o->layer_config_ranges.clear();o->layer_height_profile.clear();for(auto* v:o->volumes){v->config.reset();v->set_material_id("");v->mmu_segmentation_facets.reset();}}
                copy.plates_custom_gcodes.clear();
                copy.cad_recipe.clear();
                ensure(store_3mf(file.path().string().c_str(),&copy,nullptr,false,nullptr,true),"export_failed","Native geometry-only 3MF export failed");
            }
        } else {
            ensure(suffix==".gcode","unsupported_format","G-code destination must end in .gcode");
            ensure(!params.contains("object_id"),"invalid_params","G-code export targets a plate");
            auto& plate=resolve_plate(ctx,params);
            ensure(plate.is_slice_result_valid()&&plate.is_valid_gcode_file(),"not_sliced","Plate needs a current valid slice");
            // Copy the already generated toolpath; do not execute post-processing scripts.
            auto input=fs::path(plate.get_tmp_gcode_path());ensure(fs::is_regular_file(input),"not_sliced","Slice artifact is missing");
            fs::copy_file(input,file.path(),fs::copy_options::none);
        }
        auto out=file.publish();out["source_state_token"]=ctx.state_token;out["document_id"]=id(p.model());return out;
    }
    if(method=="plates.create") {
        ensure(list.get_plate_count()<static_cast<int>(GUI::PartPlateList::MAX_PLATES_COUNT),"limit_exceeded","Maximum plate count reached");
        auto name=params.contains("name")?text(params,"name"):std::string();
        p.take_snapshot("Agent create plate");int index=list.create_plate(true);ensure(index>=0,"operation_failed","Native plate creation failed");
        if(!name.empty())list.get_plate(index)->set_plate_name(name);
        refresh(p);return result(ctx);
    }
    if(method.rfind("plates.",0)==0) {
        auto& plate=resolve_plate(ctx,params);int index=plate.get_index();
        if(method=="plates.select") { list.select_plate(index);p.update();return result(ctx); }
        if(method=="plates.lock") {
            bool lock=params.at("locked").get<bool>();if(lock!=plate.is_locked()){p.take_snapshot("Agent lock plate");list.lock_plate(index,lock);}return result(ctx);
        }
        ensure(!plate.is_locked(),"locked","Plate is locked");
        if(method=="plates.rename") { auto name=text(params,"name");if(name!=plate.get_plate_name()){p.take_snapshot("Agent rename plate");plate.set_plate_name(name);} }
        else if(method=="plates.duplicate") {
            ensure(list.get_plate_count()<static_cast<int>(GUI::PartPlateList::MAX_PLATES_COUNT),"limit_exceeded","Maximum plate count reached");
            p.take_snapshot("Agent duplicate plate");ensure(p.duplicate_plate(index)>=0,"operation_failed","Native plate duplication failed");
        } else if(method=="plates.delete") {
            ensure(list.get_plate_count()>1,"invalid_operation","Cannot delete the last plate");
            ensure(plate.get_objects_on_this_plate().empty(),"plate_not_empty","Move or delete contents before deleting a plate");
            ensure(p.delete_plate(index)>=0,"operation_failed","Native plate deletion failed");
        } else if(method=="plates.reorder") {
            ensure(params["index"].is_number_integer(),"invalid_params","index must be an integer");int next=params["index"].get<int>();
            ensure(next>=0&&next<list.get_plate_count(),"invalid_params","Plate index is outside the list");
            if(next!=index){p.take_snapshot("Agent reorder plates");ensure(list.move_plate_to_index(index,next)>=0,"operation_failed","Native plate reorder failed");}
        }
        refresh(p);return result(ctx);
    }
    if(method=="layout.arrange") {
        ensure(params["instance_ids"].is_array()&&!params["instance_ids"].empty(),"invalid_params","Explicit instance_ids are required");
        std::vector<InstanceRef> refs;std::set<std::string> seen;
        for(const auto& value:params["instance_ids"]) { ensure(value.is_string(),"invalid_params","Instance IDs must be strings");auto key=value.get<std::string>();ensure(seen.insert(key).second,"invalid_params","Duplicate instance ID");auto r=instance_ref(p,key);unlocked(p,r);refs.push_back(r); }
        auto& selection=p.get_view3D_canvas3D()->get_selection();selection.remove_all();
        for(const auto& r:refs)selection.add_instance(static_cast<unsigned>(r.oi),static_cast<unsigned>(r.ii),false);
        p.set_prepare_state(GUI::Job::PREPARE_STATE_DEFAULT);
        auto cancel=std::make_shared<std::atomic<bool>>(false);ctx.register_cancel([cancel]{cancel->store(true);});
        auto captured=ctx;captured.state_token=ctx.current_state();
        ensure(p.get_ui_job_worker().push(std::make_shared<OwnedArrange>(captured,cancel,params["instance_ids"].get<std::vector<std::string>>())),"busy","Arrange worker is unavailable");
        return nullptr;
    }
    if(method.rfind("instances.",0)==0||method=="objects.make_independent"||method=="parts.translate"||method=="parts.rotate"||method=="parts.scale"||method=="parts.mirror") {
        const bool part=method.rfind("parts.",0)==0;
        auto operation=part?"instances."+method.substr(6):method;
        auto r=instance_ref(p,text(params,"instance_id"));unlocked(p,r);
        VolumeRef volume_target{};
        if(part){volume_target=volume_ref(p,text(params,"volume_id"));ensure(volume_target.object==r.object,"invalid_target","Volume and instance belong to different objects");ensure(mesh_revision(*volume_target.volume)==text(params,"mesh_revision"),"stale_mesh","Volume mesh revision changed");}
        auto* canvas=p.get_view3D_canvas3D();auto& selection=canvas->get_selection();
        if(operation=="instances.printable"){p.take_snapshot("Agent instance printable");r.instance->printable=params.at("printable").get<bool>();refresh(p);return result(ctx);}
        if(operation=="instances.delete") {
            p.take_snapshot("Agent delete instance");
            if(r.object->instances.size()==1)p.remove(r.oi);else {r.object->delete_instance(r.ii);touch_object(p,*r.object);}
            refresh(p);return result(ctx);
        }
        if(operation=="instances.clone") {
            p.take_snapshot("Agent clone instance");auto* clone=r.object->add_instance(*r.instance);auto created=id(*clone);touch_object(p,*r.object);refresh(p);auto out=result(ctx);out["created_instance_id"]=created;return out;
        }
        if(method=="objects.make_independent") {
            ensure(r.object->instances.size()>1,"invalid_operation","Instance is already independent");
            ensure(!r.object->is_cut(),"cut_object","Cut-linked objects must be edited through the native cut workflow");
            Model prepared;copy_materials(prepared,p.model());auto* o=prepared.add_object(*r.object);
            for(std::size_t j=o->instances.size();j>0;--j)if(j-1!=r.ii)o->delete_instance(j-1);
            auto old_instance=id(*r.instance);auto before=object_ids(p);p.take_snapshot("Agent make instance independent");r.object->delete_instance(r.ii);touch_object(p,*r.object);p.agent_import_objects(prepared.objects,r.object->auto_drop,true);refresh(p);auto out=result(ctx);out["created_object_ids"]=added(p,before);out["removed_instance_ids"]=Json::array({old_instance});return out;
        }
        Vec3d value=Vec3d::Zero();std::string frame="world";bool relative=true;
        if(operation=="instances.translate"||operation=="instances.rotate"||operation=="instances.scale") {
            value=vector(params,"value");frame=text(params,"frame");relative=params.at("relative").get<bool>();
            ensure(frame=="world"||frame=="object"||frame=="plate","invalid_params","Unknown transform frame");
            ensure(operation=="instances.translate"||frame!="plate","invalid_params","Plate frame is supported for translation only");
            ensure(operation!="instances.rotate"||relative,"unsupported_mode","Rotation currently accepts relative Euler angles only");
            ensure(operation!="instances.scale"||(value.array()>0.000001).all(),"invalid_params","Scale factors must be positive");
            ensure(operation!="instances.translate"||relative||frame!="object","unsupported_mode","Absolute translation is specified in world or plate frame");
            ensure(operation!="instances.scale"||relative||frame=="object","unsupported_mode","Absolute scale factors are specified in object frame");
            if(operation=="instances.translate"&&!relative) {
                ensure(!part,"unsupported_mode","Part translation currently accepts relative values only");
                if(frame=="plate") {int pi=list.find_instance_belongs(static_cast<int>(r.oi),static_cast<int>(r.ii));ensure(pi>=0&&pi<list.get_plate_count(),"invalid_scope","Instance is not assigned to a plate");value+=list.get_plate(pi)->get_origin();}
                value-=r.instance->get_offset();
            }
        } else if(operation=="instances.orient_to_face") {
            value=vector(params,"normal");ensure(value.norm()>1e-9,"invalid_params","Face normal must be nonzero");value.normalize();
        } else if(operation=="instances.move_to_plate") {
            auto& destination=resolve_plate(ctx,params);ensure(!destination.is_locked(),"locked","Destination plate is locked");value=vector(params,"position")+destination.get_origin()-r.instance->get_offset();
        }
        Axis mirror_axis=X;
        if(operation=="instances.mirror") {auto axis=text(params,"axis");ensure(axis=="x"||axis=="y"||axis=="z","invalid_params","Unknown mirror axis");mirror_axis=axis=="x"?X:axis=="y"?Y:Z;}
        if(operation=="instances.center"){int pi=list.find_instance_belongs(static_cast<int>(r.oi),static_cast<int>(r.ii));ensure(pi>=0&&pi<list.get_plate_count(),"invalid_scope","Instance has no plate");}
        select(p,r);if(part){selection.remove_all();selection.add_volume(static_cast<unsigned>(r.oi),static_cast<unsigned>(volume_target.vi),static_cast<int>(r.ii));selection.setup_cache();}p.take_snapshot("Agent transform instance");
        if(params.contains("auto_drop"))r.object->auto_drop=params.at("auto_drop").get<bool>();
        GUI::TransformationType type(GUI::TransformationType::World_Relative_Joint);
        if(frame=="object")type.set_instance();
        if(operation=="instances.translate"||operation=="instances.move_to_plate") {selection.translate(value,type);canvas->do_move("");}
        else if(operation=="instances.orient_to_face"){selection.flattening_rotate(value);canvas->do_rotate("");}
        else if(operation=="instances.rotate") {
            // Native rotate expects one changing component when synchronizing linked instances.
            for(int axis=0;axis<3;++axis)if(value[axis]!=0){selection.setup_cache();Vec3d delta=Vec3d::Zero();delta[axis]=value[axis]*(PI/180.0);selection.rotate(delta,type);canvas->do_rotate("");}
        }
        else if(operation=="instances.scale") {if(!relative){type.set_instance();type.set_absolute();}selection.scale(value,type);canvas->do_scale("");}
        else if(operation=="instances.mirror") {selection.mirror(mirror_axis,type);canvas->do_scale("");}
        else if(operation=="instances.center") {
            int pi=list.find_instance_belongs(static_cast<int>(r.oi),static_cast<int>(r.ii));
            ensure(pi>=0&&pi<list.get_plate_count(),"invalid_scope","Instance has no plate");
            auto center=list.get_plate(pi)->get_center_origin();auto source=selection.get_bounding_box().center();
            selection.translate(Vec3d(center.x()-source.x(),center.y()-source.y(),0),type);canvas->do_move("");
        }
        else if(operation=="instances.drop") {selection.translate(Vec3d(0,0,-selection.get_drop_min_z()),type);canvas->do_move("");}
        refresh(p);auto out=result(ctx);Json affected=Json::array();for(auto* i:r.object->instances)affected.push_back(id(*i));out["affected_instance_ids"]=affected;return out;
    }
    if(method=="objects.rename"||method=="objects.printable"||method=="objects.delete"||method=="objects.split"||method=="objects.clone") {
        auto r=object_ref(p,text(params,"object_id"));
        for(std::size_t i=0;i<r.object->instances.size();++i)unlocked(p,{r.object,r.object->instances[i],r.index,i});
        if(method=="objects.clone"){Model prepared;copy_materials(prepared,p.model());prepared.add_object(*r.object);auto before=object_ids(p);p.take_snapshot("Agent clone object");p.agent_import_objects(prepared.objects,r.object->auto_drop,true);refresh(p);auto out=result(ctx);out["created_object_ids"]=added(p,before);return out;}
        if(method=="objects.rename") {auto name=text(params,"name");p.take_snapshot("Agent rename object");r.object->name=name;refresh(p);return result(ctx);}
        if(method=="objects.printable") {bool printable=params.at("printable").get<bool>();p.take_snapshot("Agent set printable");r.object->printable=printable;refresh(p);return result(ctx);}
        if(method=="objects.delete") {p.take_snapshot("Agent delete object");p.remove(r.index);refresh(p);return result(ctx);}
        ensure(!r.object->is_cut(),"cut_object","Cut-linked objects cannot be split independently");
        Model prepared=p.model();ModelObjectPtrs parts;prepared.objects[r.index]->split(&parts,true);
        ensure(parts.size()>1,"not_splittable","Object has only one connected printable part");
        auto before=object_ids(p);auto old=id(*r.object);p.take_snapshot("Agent split to objects");p.apply_cut_object_to_model(r.index,parts,false,true);refresh(p);auto out=result(ctx);out["created_object_ids"]=added(p,before);out["removed_object_ids"]=Json::array({old});out["modifier_policy"]="native_split_drops_non_model_volumes";return out;
    }
    if(method=="parts.split") {
        auto r=volume_ref(p,text(params,"volume_id"));ensure(mesh_revision(*r.volume)==text(params,"mesh_revision"),"stale_mesh","Volume topology changed");
        ensure(r.volume->is_model_part(),"invalid_scope","Only model parts can be split");ensure(!r.object->is_cut(),"cut_object","Cut-linked objects cannot be split independently");
        ensure(r.volume->is_splittable(),"not_splittable","Part has one connected component");
        for(std::size_t i=0;i<r.object->instances.size();++i)unlocked(p,{r.object,r.object->instances[i],r.oi,i});
        Model prepared=p.model();auto* clone=prepared.objects[r.oi];clone->volumes[r.vi]->split(1,true);
        const std::string old=id(*r.volume);
        const std::size_t split_count=clone->volumes.size()-r.object->volumes.size()+1;
        std::vector<ModelVolume*> new_parts;new_parts.reserve(split_count);
        p.take_snapshot("Agent split to parts");
        for(std::size_t i=0;i<split_count;++i)new_parts.push_back(r.object->add_volume(*clone->volumes[r.vi+i]));
        r.object->delete_volume(r.vi);
        auto& live=r.object->volumes;
        live.erase(live.end()-static_cast<std::ptrdiff_t>(split_count),live.end());
        live.insert(live.begin()+static_cast<std::ptrdiff_t>(r.vi),new_parts.begin(),new_parts.end());
        touch_object(p,*r.object);refresh(p);
        auto out=result(ctx);Json created=Json::array();for(const auto* v:new_parts)created.push_back(id(*v));
        out["removed_volume_ids"]=Json::array({old});out["created_volume_ids"]=created;out["volume_replacements"]={{old,created}};out["object_id"]=id(*r.object);return out;
    }
    if(method=="objects.merge") {
        const auto& ids=params.at("object_ids");ensure(ids.is_array()&&ids.size()>=2&&ids.size()<=64,"invalid_params","Merge requires 2 to 64 objects");
        std::vector<ObjectRef> refs;std::set<std::string> unique;int plate=-2;
        for(const auto& key:ids){ensure(key.is_string(),"invalid_params","Object IDs must be strings");auto wanted=key.get<std::string>();ensure(unique.insert(wanted).second,"invalid_params","Duplicate object ID");auto r=object_ref(p,wanted);ensure(r.object->instances.size()==1,"invalid_scope","Make objects independent before merging multiple-instance objects");unlocked(p,{r.object,r.object->instances[0],r.index,0});int pi=list.find_instance_belongs(static_cast<int>(r.index),0);if(plate==-2)plate=pi;ensure(plate==pi,"invalid_scope","Merged objects must be on the same plate");refs.push_back(r);}
        Model prepared;copy_materials(prepared,p.model());auto* merged=prepared.add_object();merged->name="Assembly";merged->add_instance();
        // Distinct object-level overrides cannot all survive a multipart merge.
        const auto& base=refs.front().object->config.get();
        for(const auto& r:refs)ensure(r.object->config.get()==base&&r.object->layer_config_ranges.empty()&&r.object->layer_height_profile.empty()&&!r.object->is_cut(),"settings_conflict","Merge requires equal object overrides, no height ranges/profile and no cut linkage");
        merged->config.assign_config(base);
        for(const auto& r:refs){auto* o=r.object;for(const auto* v:o->volumes){auto* added_volume=merged->add_volume(*v);added_volume->set_transformation(o->instances[0]->get_matrix()*v->get_matrix());}merged->printable=merged->printable&&o->printable&&o->instances[0]->printable;merged->auto_drop=merged->auto_drop&&o->auto_drop;}
        auto before=object_ids(p);p.take_snapshot("Agent merge multipart");std::sort(refs.begin(),refs.end(),[](const ObjectRef&a,const ObjectRef&b){return a.index>b.index;});for(const auto&r:refs)p.remove(r.index);p.agent_import_objects(prepared.objects,merged->auto_drop,true);refresh(p);auto out=result(ctx);out["created_object_ids"]=added(p,before);out["removed_object_ids"]=ids;return out;
    }
    if(method=="modifiers.add") {
        auto r=object_ref(p,text(params,"object_id"));for(std::size_t i=0;i<r.object->instances.size();++i)unlocked(p,{r.object,r.object->instances[i],r.index,i});
        auto size=vector(params,"size"),position=vector(params,"position");ensure((size.array()>0).all(),"invalid_params","Primitive size must be positive");auto shape=text(params,"shape"),kind=text(params,"type");
        ModelVolumeType type=ModelVolumeType::PARAMETER_MODIFIER;
        if(kind=="negative")type=ModelVolumeType::NEGATIVE_VOLUME;else if(kind=="support_blocker")type=ModelVolumeType::SUPPORT_BLOCKER;else if(kind=="support_enforcer")type=ModelVolumeType::SUPPORT_ENFORCER;else ensure(kind=="modifier","invalid_params","Unknown modifier type");
        TriangleMesh mesh;
        if(shape=="box"){mesh=make_cube(size.x(),size.y(),size.z());mesh.translate((-0.5*size).cast<float>());}
        else if(shape=="sphere"){mesh=make_sphere(1);mesh.scale((0.5*size).cast<float>());}
        else if(shape=="cylinder"){mesh=make_cylinder(1,size.z());mesh.scale(Vec3f(static_cast<float>(size.x()/2),static_cast<float>(size.y()/2),1));mesh.translate(0,0,-size.z()/2);}
        else throw DomainError("invalid_params","Unknown primitive shape");
        auto name=params.contains("name")?text(params,"name"):kind;
        p.take_snapshot("Agent add modifier");auto* volume=r.object->add_volume(std::move(mesh),type,false);volume->name=name;volume->set_offset(position);touch_object(p,*r.object);refresh(p);auto out=result(ctx);out["created_volume_id"]=id(*volume);return out;
    }
    if(method=="modifiers.set_type") {
        auto r=volume_ref(p,text(params,"volume_id"));ensure(!r.volume->is_model_part(),"invalid_scope","Model parts cannot be retyped by the modifiers command");
        for(std::size_t i=0;i<r.object->instances.size();++i)unlocked(p,{r.object,r.object->instances[i],r.oi,i});
        auto kind=text(params,"type");ModelVolumeType type=ModelVolumeType::PARAMETER_MODIFIER;
        if(kind=="negative")type=ModelVolumeType::NEGATIVE_VOLUME;else if(kind=="support_blocker")type=ModelVolumeType::SUPPORT_BLOCKER;else if(kind=="support_enforcer")type=ModelVolumeType::SUPPORT_ENFORCER;else ensure(kind=="modifier","invalid_params","Unknown modifier type");
        p.take_snapshot("Agent modifier type");r.volume->set_type(type);touch_object(p,*r.object);refresh(p);return result(ctx);
    }
    if(method=="modifiers.delete") {
        auto r=volume_ref(p,text(params,"volume_id"));ensure(!r.volume->is_model_part(),"invalid_scope","Use object deletion for printable model parts");for(std::size_t i=0;i<r.object->instances.size();++i)unlocked(p,{r.object,r.object->instances[i],r.oi,i});p.take_snapshot("Agent delete modifier");r.object->delete_volume(r.vi);touch_object(p,*r.object);refresh(p);return result(ctx);
    }
    throw DomainError("method_not_found","Unknown scene command");
}
} // namespace Slic3r::AgentControl
