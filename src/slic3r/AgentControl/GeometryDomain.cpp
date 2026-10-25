#include "libslic3r/ObjectID.hpp"
#include "libslic3r/libslic3r.h"
#include <initializer_list>
#include "Domain.hpp"
#include "Hooks.hpp"
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/CutUtils.hpp"
#include "libslic3r/Geometry.hpp"
#include "libslic3r/MeshBoolean.hpp"
#include "libslic3r/MeshRepair.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/QuadricEdgeCollapse.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/TriangleMeshSlicer.hpp"
#include "libslic3r/TriangleSelector.hpp"
#include "libslic3r/ExPolygon.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include "slic3r/GUI/Gizmos/GLGizmoCut.hpp"
#include "slic3r/GUI/Jobs/Job.hpp"
#include "slic3r/GUI/Jobs/Worker.hpp"
#include <Eigen/Geometry>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r::AgentControl {
namespace {
std::string id(const ObjectBase& object) { return std::to_string(object.id().id); }
Json text_schema() { return {{"type","string"},{"minLength",1},{"maxLength",128}}; }
Json vector_schema() { return {{"type","array"},{"minItems",3},{"maxItems",3},{"items",{{"type","number"},{"minimum",-1e6},{"maximum",1e6}}}}; }
Json choice(std::initializer_list<const char*> values) { Json v=Json::array();for(auto s:values)v.push_back(s);return {{"type","string"},{"enum",v}}; }
Json vec(const Vec3d& v) { return Json::array({v.x(),v.y(),v.z()}); }
Vec3d vector(const Json& value) {
    ensure(value.is_array()&&value.size()==3,"invalid_params","Expected three finite numbers");
    Vec3d out;
    for(int i=0;i<3;++i) { ensure(value[i].is_number(),"invalid_params","Expected finite numbers");out[i]=value[i].get<double>();ensure(std::isfinite(out[i])&&std::abs(out[i])<=1e6,"invalid_params","Coordinate outside supported range"); }
    return out;
}
struct ObjectRef { ModelObject* object; std::size_t index; };
ObjectRef object_ref(GUI::Plater& plater,const std::string& wanted) {
    for(std::size_t i=0;i<plater.model().objects.size();++i) if(id(*plater.model().objects[i])==wanted)return {plater.model().objects[i],i};
    throw DomainError("not_found","Object ID no longer exists");
}
std::size_t volume_index(const ModelObject& object,const std::string& wanted) {
    for(std::size_t i=0;i<object.volumes.size();++i)if(id(*object.volumes[i])==wanted)return i;
    throw DomainError("not_found","Volume ID does not belong to the requested object");
}
void check_revisions(const ModelObject& object,const Json& revisions) {
    ensure(revisions.is_object()&&revisions.size()==object.volumes.size(),"stale_mesh","Supply mesh_revisions for every volume of the object");
    for(auto* volume:object.volumes)ensure(revisions.contains(id(*volume))&&revisions[id(*volume)]==mesh_revision(*volume),"stale_mesh","A volume mesh or its local transform changed");
}
void unlocked(GUI::Plater& plater,const ObjectRef& ref) {
    auto& plates=plater.get_partplate_list();
    for(std::size_t i=0;i<ref.object->instances.size();++i) {
        const int p=plates.find_instance_belongs(static_cast<int>(ref.index),static_cast<int>(i));
        ensure(p<0||!plates.get_plate(p)->is_locked(),"locked","A linked instance belongs to a locked plate");
    }
}
void valid_transform(const Transform3d& transform) {
    ensure(transform.matrix().allFinite()&&std::isfinite(transform.linear().determinant())&&
           std::abs(transform.linear().determinant())>1e-12,"invalid_geometry","Geometry operation requires a finite, invertible transform");
}
void valid_mesh(const TriangleMesh& mesh) {
    ensure(!mesh.empty(),"invalid_geometry","Mesh is empty");
    for(const auto& v:mesh.its.vertices)ensure(v.allFinite(),"invalid_geometry","Mesh contains non-finite vertices");
    for(const auto& f:mesh.its.indices)for(int k=0;k<3;++k)ensure(f[k]>=0&&static_cast<std::size_t>(f[k])<mesh.its.vertices.size(),"invalid_geometry","Mesh has an invalid vertex index");
}
Json metrics(const TriangleMesh& mesh,bool deep) {
    valid_mesh(mesh);
    const auto box=mesh.bounding_box();
    const double volume=its_volume(mesh.its);
    ensure(box.min.allFinite()&&box.max.allFinite()&&std::isfinite(volume),"invalid_geometry","Mesh bounds or volume overflow native numeric limits");
    Json result={{"vertices",mesh.its.vertices.size()},{"triangles",mesh.its.indices.size()},
        {"open_edges",its_num_open_edges(mesh.its)},{"signed_volume_mm3",volume},
        {"bounds",{{"min",vec(box.min)},{"max",vec(box.max)}}}};
    std::size_t degenerate=0;
    for(const auto& face:mesh.its.indices) {
        const Vec3d a=mesh.its.vertices[face[0]].cast<double>();
        const Vec3d b=mesh.its.vertices[face[1]].cast<double>();
        const Vec3d c=mesh.its.vertices[face[2]].cast<double>();
        if((b-a).cross(c-a).squaredNorm()<1e-20)++degenerate;
    }
    result["degenerate_triangles"]=degenerate;
    if(deep)result["self_intersects"]=MeshBoolean::cgal::does_self_intersect(mesh);
    return result;
}
struct Work {
    std::shared_ptr<Model> source;
    std::shared_ptr<Model> result;
    std::string operation;
    Json params;
    Json report;
    Json input_volume_ids=Json::array();
    bool changed=true;
    std::size_t instance=0;
    std::size_t volume=0;
    std::size_t tool=0;
    Transform3d cut_matrix=Transform3d::Identity();
    Transform3d rotation=Transform3d::Identity();
    std::shared_ptr<std::atomic<bool>> cancelled=std::make_shared<std::atomic<bool>>(false);
};
void check_cancel(const Work& work,const GUI::Job::Ctl& ctl) {
    if(work.cancelled->load(std::memory_order_relaxed)||ctl.was_canceled())throw DomainError("cancelled","Geometry operation was cancelled");
}
void replace_mesh(ModelVolume& volume,TriangleMesh mesh,bool keep_paint) {
    auto saved=keep_paint?volume.save_painting():std::optional<TriangleSelector::SavedPainting>{};
    volume.set_mesh(std::move(mesh));
    volume.source=ModelVolume::Source();
    volume.calculate_convex_hull();
    volume.invalidate_convex_hull_2d();
    volume.set_new_unique_id();
    volume.restore_painting(saved);
}
void prepare_connectors(Work& work,ModelObject& object,const GUI::Job::Ctl& ctl) {
    const auto& params=work.params;
    if(!params.contains("connectors")||params["connectors"].empty())return;
    ensure(params.at("keep")=="both"&&!params.value("as_parts",false),"invalid_params","Connectors require both halves kept as separate objects");
    const Vec3d offset=object.instances[work.instance]->get_offset();
    const Transform3d to_cut=work.cut_matrix.inverse()*object.instances[work.instance]->get_transformation().get_matrix_no_offset();
    TriangleMesh mesh;
    for(auto* volume:object.volumes)if(volume->is_model_part()) {
        TriangleMesh part=volume->mesh();part.transform(to_cut*volume->get_matrix(),true);mesh.merge(part);
    }
    // Use the same section-contour containment and connector-overlap restrictions
    // as the gizmo, without its live clipper/selection state.
    auto sections=slice_mesh_ex(mesh.its,std::vector<float>{0.0f});
    ensure(!sections.empty()&&!sections.front().empty(),"invalid_geometry","Cut plane does not cross a printable volume");
    for(const auto& entry:params["connectors"]) {
        check_cancel(work,ctl);
        Vec3d world=vector(entry.at("position_mm"));
        Vec3d cut_position=work.cut_matrix.inverse()*(world-offset);
        ensure(std::abs(cut_position.z())<1e-4,"invalid_params","Connector position must lie on the explicit cut plane");
        const double radius=entry.at("radius_mm").get<double>();
        const double depth=entry.at("depth_mm").get<double>();
        const auto box=mesh.bounding_box();
        ensure(box.min.z()<=-depth&&box.max.z()>=depth,"invalid_geometry","Connector depth exceeds the printable geometry bounds on a cut side");
        for(int i=0;i<60;++i) {
            const double angle=2.0*PI*i/60;
            const Vec2d point=cut_position.head<2>()+radius*Vec2d(std::cos(angle),std::sin(angle));
            bool inside=false;
            for(const auto& contour:sections.front())if(contour.contains(Point::new_scale(point.x(),point.y()))){inside=true;break;}
            ensure(inside,"invalid_geometry","Connector footprint lies outside the cut section");
        }
        for(const auto& other:object.cut_connectors)ensure((world-offset-other.pos).norm()>=radius+other.radius,"invalid_geometry","Connector footprints overlap");
        const std::string type=entry.at("type"),style=entry.at("style"),shape=entry.at("shape");
        CutConnectorAttributes attrs(type=="plug"?CutConnectorType::Plug:type=="dowel"?CutConnectorType::Dowel:CutConnectorType::Snap,
            style=="prism"?CutConnectorStyle::Prism:CutConnectorStyle::Frustum,
            shape=="circle"?CutConnectorShape::Circle:shape=="triangle"?CutConnectorShape::Triangle:shape=="square"?CutConnectorShape::Square:CutConnectorShape::Hexagon);
        ensure(attrs.type!=CutConnectorType::Snap||(attrs.style==CutConnectorStyle::Prism&&attrs.shape==CutConnectorShape::Circle),"invalid_params","Snap connectors use a circular prism");
        object.cut_connectors.emplace_back(world-offset,work.rotation,static_cast<float>(radius),entry.at("depth_mm").get<float>(),
            entry.value("radius_tolerance_mm",0.0f),entry.value("depth_tolerance_mm",0.1f),
            static_cast<float>(entry.value("angle_degrees",0.0)*PI/180.0),attrs);
    }
}
void compute(Work& work,GUI::Job::Ctl& ctl) {
    check_cancel(work,ctl);
    ModelObject& source=*work.source->objects.front();
    const auto& p=work.params;
    for(auto* volume:source.volumes){valid_mesh(volume->mesh());valid_transform(volume->get_matrix());}
    for(auto* instance:source.instances)valid_transform(instance->get_matrix());
    ctl.update_status(0,"Computing geometry on a detached model");
    if(work.operation=="validate") {
        work.report=Json::array();
        for(std::size_t i=0;i<source.volumes.size();++i){check_cancel(work,ctl);work.report.push_back({{"volume_id",work.input_volume_ids[i]},{"mesh",metrics(source.volumes[i]->mesh(),p.value("deep",false))}});}
        return;
    }
    work.result=std::make_shared<Model>();
    for(const auto& material:work.source->materials)work.result->add_material(material.first,*material.second);
    const bool keep_paint=p.value("keep_paint",true);
    if(work.operation=="cut") {
        double below=std::numeric_limits<double>::infinity(),above=-below;
        const Transform3d instance=source.instances[work.instance]->get_transformation().get_matrix_no_offset();
        for(const auto* volume:source.volumes)if(volume->is_model_part()) {
            const Transform3d to_cut=work.cut_matrix.inverse()*instance*volume->get_matrix();
            for(const auto& vertex:volume->mesh().its.vertices) {
                const double height=(to_cut*vertex.cast<double>()).z();
                below=std::min(below,height);above=std::max(above,height);
            }
        }
        ensure(below < -1e-6 && above > 1e-6,"invalid_geometry","Cut plane must pass through the object's printable geometry");
        prepare_connectors(work,source,ctl);
        const bool connectors=!source.cut_connectors.empty();
        const int dowels=GUI::prepare_cut_connectors(source,work.rotation,work.rotation.linear().col(2),"Connector",
            p.value("snap_space_proportion",0.3),p.value("snap_bulge_proportion",0.15));
        const std::string keep=p.at("keep");
        ModelObjectCutAttributes attributes=ModelObjectCutAttribute::InvalidateCutInfo;
        if(keep=="both"||keep=="upper")attributes=attributes|ModelObjectCutAttribute::KeepUpper;
        if(keep=="both"||keep=="lower")attributes=attributes|ModelObjectCutAttribute::KeepLower;
        if(connectors)attributes=ModelObjectCutAttribute::KeepUpper|ModelObjectCutAttribute::KeepLower;
        if(p.value("as_parts",false))attributes=attributes|ModelObjectCutAttribute::KeepAsParts;
        if(p.value("flip_upper",false))attributes=attributes|ModelObjectCutAttribute::FlipUpper;
        if(p.value("flip_lower",false))attributes=attributes|ModelObjectCutAttribute::FlipLower;
        if(p.value("place_on_cut_upper",false))attributes=attributes|ModelObjectCutAttribute::PlaceOnCutUpper;
        if(p.value("place_on_cut_lower",false))attributes=attributes|ModelObjectCutAttribute::PlaceOnCutLower;
        if(keep_paint)attributes=attributes|ModelObjectCutAttribute::KeepPaint;
        if(dowels)attributes=attributes|ModelObjectCutAttribute::CreateDowels;
        GUI::update_object_cut_id(source.cut_id,attributes,dowels);
        Cut cut(&source,static_cast<int>(work.instance),work.cut_matrix,attributes);
        const auto& result=cut.perform_with_plane();
        ensure(!result.empty(),"empty_result","The cut produced no objects");
        for(auto* object:result) {
            auto* prepared=work.result->add_object(*object);
            prepared->ensure_on_bed();
        }
    } else {
        auto* object=work.result->add_object(source);
        auto* volume=object->volumes[work.volume];
        if(work.operation=="simplify") {
            indexed_triangle_set mesh=volume->mesh().its;
            float max_error=p.value("max_error",std::numeric_limits<float>::max());
            its_quadric_edge_collapse(mesh,p.at("triangle_count").get<std::uint32_t>(),&max_error,
                [&](){check_cancel(work,ctl);},[&](int percent){ctl.update_status(percent,"Simplifying mesh");});
            const auto& old=volume->mesh().its;
            const bool same=old.vertices.size()==mesh.vertices.size()&&old.indices.size()==mesh.indices.size()&&
                std::equal(old.vertices.begin(),old.vertices.end(),mesh.vertices.begin(),[](const Vec3f& a,const Vec3f& b){return (a.array()==b.array()).all();})&&
                std::equal(old.indices.begin(),old.indices.end(),mesh.indices.begin(),[](const Vec3i32& a,const Vec3i32& b){return (a.array()==b.array()).all();});
            work.changed=!same;
            TriangleMesh result(std::move(mesh));valid_mesh(result);
            if(work.changed)replace_mesh(*volume,std::move(result),keep_paint);
        } else if(work.operation=="repair") {
            MeshRepairStats stats;std::string error;
            const bool ok=volume->repair(keep_paint,&stats,&error,[&](int percent){check_cancel(work,ctl);ctl.update_status(percent,"Repairing mesh");});
            ensure(ok,"invalid_geometry",error.empty()?"No solid could be built from this mesh":error.c_str());
            work.changed=stats.modified;
            work.report["repair"]={{"modified",stats.modified},{"welded_vertices",stats.welded_vertices},
                {"removed_triangles",stats.removed_triangles},{"filled_holes",stats.filled_holes},{"reversed_shells",stats.reversed_shells}};
        } else if(work.operation=="boolean") {
            TriangleMesh a=volume->mesh(),b=object->volumes[work.tool]->mesh();
            a.transform(volume->get_matrix(),true);b.transform(object->volumes[work.tool]->get_matrix(),true);
            const std::string operation=p.at("operation");
            for(const auto* mesh:{&a,&b}) {
                const Json info=metrics(*mesh,false);
                ensure(info["open_edges"]==0&&info["degenerate_triangles"]==0&&std::abs(info["signed_volume_mm3"].get<double>())>1e-9,
                    "invalid_geometry","Boolean inputs must be closed, nondegenerate solids");
                check_cancel(work,ctl);
                ensure(!MeshBoolean::cgal::does_self_intersect(*mesh),"invalid_geometry","Repair self-intersecting boolean operands first");
            }
            std::vector<TriangleMesh> output;
            MeshBoolean::mcut::make_boolean(a,b,output,operation=="union"?"UNION":operation=="difference"?"A_NOT_B":"INTERSECTION");
            TriangleMesh result;
            for(const auto& mesh:output)result.merge(mesh);
            ensure(!result.empty(),"empty_result","Boolean result is empty; the document was not changed");
            ensure(its_num_open_edges(result.its)==0,"invalid_geometry","Boolean backend returned an open mesh; no change was committed");
            result.transform(volume->get_matrix().inverse(),true);
            replace_mesh(*volume,std::move(result),keep_paint);
            if(keep_paint&&operation!="difference") {
                auto saved=object->volumes[work.tool]->save_painting();
                if(saved)saved->mesh.transform(volume->get_matrix().inverse()*object->volumes[work.tool]->get_matrix(),true);
                volume->restore_painting(saved,true);
            }
            if(p.value("delete_tool",true))object->delete_volume(work.tool);
        }
        object->invalidate_bounding_box();
    }
    check_cancel(work,ctl);
    work.report["objects"]=Json::array();
    for(auto* object:work.result->objects) {
        Json volumes=Json::array();
        for(auto* volume:object->volumes){valid_mesh(volume->mesh());
            if(work.operation=="cut"&&volume->is_model_part())ensure(its_num_open_edges(volume->mesh().its)==0,"invalid_geometry","Cut returned an open solid; repair the source mesh before cutting");
            volumes.push_back({{"mesh",metrics(volume->mesh(),false)}});}
        work.report["objects"].push_back({{"volumes",volumes},{"instance_count",object->instances.size()}});
    }
    ctl.update_status(100,"Geometry computed");
}
Json connector_schema() {
    Json properties={{"position_mm",vector_schema()},{"type",choice({"plug","dowel","snap"})},{"style",choice({"prism","frustum"})},
        {"shape",choice({"circle","triangle","square","hexagon"})},
        {"radius_mm",{{"type","number"},{"minimum",0.01},{"maximum",1000}}},
        {"depth_mm",{{"type","number"},{"minimum",0.01},{"maximum",1000}}},
        {"radius_tolerance_mm",{{"type","number"},{"minimum",0},{"maximum",1}}},
        {"depth_tolerance_mm",{{"type","number"},{"minimum",0},{"maximum",1}}},
        {"angle_degrees",{{"type","number"},{"minimum",-360},{"maximum",360}}}};
    return {{"type","object"},{"additionalProperties",false},{"properties",properties},
        {"required",{"position_mm","type","style","shape","radius_mm","depth_mm"}}};
}
} // namespace

CommandSpecs geometry_commands() {
    Json common={{"object_id",text_schema()},{"mesh_revisions",{{"type","object"},{"additionalProperties",{{"type","string"}}}}}};
    Json options=common;
    options["volume_id"]=text_schema();options["keep_paint"]={{"type","boolean"}};options["preview"]={{"type","boolean"}};
    CommandSpecs specs;
    Json validate=common;validate["deep"]={{"type","boolean"}};
    specs["geometry.validate"]=command_spec(false,validate,{"object_id","mesh_revisions"});
    Json cut=common;
    cut["instance_id"]=text_schema();cut["frame"]=choice({"world"});cut["point_mm"]=vector_schema();cut["normal"]=vector_schema();cut["keep"]=choice({"upper","lower","both"});
    for(const auto* key:{"as_parts","flip_upper","flip_lower","place_on_cut_upper","place_on_cut_lower","keep_paint","preview"})cut[key]={{"type","boolean"}};
    cut["connectors"]={{"type","array"},{"maxItems",128},{"items",connector_schema()}};
    cut["snap_space_proportion"]={{"type","number"},{"minimum",0.01},{"maximum",0.9}};
    cut["snap_bulge_proportion"]={{"type","number"},{"minimum",0.01},{"maximum",0.9}};
    specs["geometry.cut"]=command_spec(true,cut,{"object_id","instance_id","mesh_revisions","frame","point_mm","normal","keep"});
    Json boolean=options;boolean["tool_volume_id"]=text_schema();boolean["operation"]=choice({"union","difference","intersection"});boolean["delete_tool"]={{"type","boolean"}};
    specs["geometry.boolean"]=command_spec(true,boolean,{"object_id","volume_id","tool_volume_id","mesh_revisions","operation"});
    Json simplify=options;simplify["triangle_count"]={{"type","integer"},{"minimum",4},{"maximum",10000000}};simplify["max_error"]={{"type","number"},{"minimum",0},{"maximum",1e12}};
    specs["geometry.simplify"]=command_spec(true,simplify,{"object_id","volume_id","mesh_revisions","triangle_count"});
    specs["geometry.repair"]=command_spec(true,options,{"object_id","volume_id","mesh_revisions"});
    specs["geometry.cut"].schema["description"]="Native planar cut in world millimetres for an explicit instance; object geometry changes affect every linked instance. Connectors require both separate halves. preview computes metrics without changing the document.";
    specs["geometry.boolean"].schema["description"]="Native union, difference or intersection of two positive parts within one object. Source settings are retained; delete_tool controls removal of the second part. An empty result is rejected. preview does not commit.";
    specs["geometry.simplify"].schema["description"]="Native quadric simplification of one volume, affecting all linked instances. triangle_count is a target; max_error is the native quadric metric, not a distance in millimetres. preview does not commit.";
    specs["geometry.repair"].schema["description"]="Native mesh repair of one volume on a detached model. No change creates no Undo entry. preview returns repair statistics without committing.";
    specs["geometry.validate"].schema["description"]="Validate all explicitly revision-checked object volumes; report finite/index validity, bounds, triangle count, open edges and signed volume. deep additionally checks CGAL self intersections. Does not modify geometry.";
    return specs;
}

Json execute_geometry(DomainContext& context,const std::string& method,const Json& params) {
    const auto specs=geometry_commands();auto found=specs.find(method);
    ensure(found!=specs.end(),"unknown_method","Unknown geometry command");validate_parameters(params,found->second);
    ObjectRef ref=object_ref(context.plater,params.at("object_id"));
    check_revisions(*ref.object,params.at("mesh_revisions"));
    const bool mutating=method!="geometry.validate";
    if(mutating) {
        unlocked(context.plater,ref);
        ensure(!ref.object->is_cut()&&ref.object->cut_connectors.empty(),"invalid_geometry","Existing cut correspondence must be resolved before editing geometry");
        for(auto* volume:ref.object->volumes)ensure(!volume->is_cut_connector(),"invalid_geometry","Existing cut connectors cannot be edited by this command");
    }
    auto work=std::make_shared<Work>();work->params=params;work->operation=method.substr(9);
    work->source=std::make_shared<Model>();
    for(const auto& material:context.plater.model().materials)work->source->add_material(material.first,*material.second);
    work->source->add_object(*ref.object);
    for(auto* volume:ref.object->volumes)work->input_volume_ids.push_back(id(*volume));
    if(method=="geometry.cut") {
        ensure(!params.value("as_parts",false)||params.at("keep")=="both","invalid_params","Keeping cut pieces as parts requires both halves");
        bool matched=false;
        for(std::size_t i=0;i<ref.object->instances.size();++i)if(id(*ref.object->instances[i])==params.at("instance_id")){work->instance=i;matched=true;break;}
        ensure(matched,"not_found","Instance does not belong to the explicit object");
        Vec3d normal=vector(params.at("normal"));ensure(normal.norm()>1e-9,"invalid_params","Cut normal cannot be zero");normal.normalize();
        work->rotation.linear()=Eigen::Quaterniond::FromTwoVectors(Vec3d::UnitZ(),normal).toRotationMatrix();
        work->cut_matrix=Geometry::translation_transform(vector(params.at("point_mm"))-ref.object->instances[work->instance]->get_offset())*work->rotation;
    } else if(mutating) {
        work->volume=volume_index(*ref.object,params.at("volume_id"));
        if(method=="geometry.boolean") {
            work->tool=volume_index(*ref.object,params.at("tool_volume_id"));
            ensure(work->volume!=work->tool,"invalid_params","Boolean operands must be different volumes");
            ensure(ref.object->volumes[work->volume]->is_model_part()&&ref.object->volumes[work->tool]->is_model_part(),"invalid_params","Boolean requires two positive model parts within one object");
        }
    }
    const bool preview=params.value("preview",false);
    const std::string object_id=params.at("object_id");
    const Json original_instances=[&](){Json r=Json::array();for(auto* i:ref.object->instances)r.push_back(id(*i));return r;}();
    auto cancelled=work->cancelled;
    context.register_cancel([cancelled](){cancelled->store(true,std::memory_order_relaxed);});
    Json affected_plates=Json::array();
    std::set<int> plate_indices;
    auto& plates=context.plater.get_partplate_list();
    for(std::size_t i=0;i<ref.object->instances.size();++i) {
        const int plate=plates.find_instance_belongs(static_cast<int>(ref.index),static_cast<int>(i));
        if(plate>=0&&plate_indices.insert(plate).second)affected_plates.push_back(plate_identity(context,*plates.get_plate(plate)));
    }
    const bool queued=GUI::queue_job(context.plater.get_ui_job_worker(),
        [work](GUI::Job::Ctl& ctl){compute(*work,ctl);},
        [work,ctx=context,object_id,original_instances,affected_plates,mutating,preview](bool cancelled,std::exception_ptr& exception) mutable {
            auto lifetime=ctx.lifetime.lock();
            if(!lifetime){exception=nullptr;return;}
            try {
                if(exception){auto error=exception;exception=nullptr;std::rethrow_exception(error);}
                ensure(!cancelled&&!work->cancelled->load(std::memory_order_relaxed),"cancelled","Geometry operation was cancelled");
                ensure(ctx.current_state()==ctx.state_token,"conflict","Document changed while geometry was computed");
                Json result={{"input_state_token",ctx.state_token},{"document_epoch",ctx.document_epoch},{"operation",work->operation},{"preview",preview},{"changed",mutating&&!preview&&work->changed},{"report",work->report}};
                if(mutating&&!preview&&work->changed) {
                    ObjectRef current=object_ref(ctx.plater,object_id);unlocked(ctx.plater,current);
                    check_revisions(*current.object,work->params.at("mesh_revisions"));
                    std::set<std::string> before;
                    for(auto* object:ctx.plater.model().objects)before.insert(id(*object));
                    OperationBlock guard(&ctx.plater);
                    GUI::Plater::TakeSnapshot snapshot(&ctx.plater,"Agent geometry: "+work->operation);
                    ctx.plater.apply_cut_object_to_model(current.index,work->result->objects,false,true);
                    Json created=Json::array(),mapping=Json::array();
                    for(auto* object:ctx.plater.model().objects)if(!before.count(id(*object))) {
                        Json volumes=Json::array(),instances=Json::array();
                        for(auto* v:object->volumes)volumes.push_back({{"volume_id",id(*v)},{"mesh_revision",mesh_revision(*v)}});
                        for(auto* i:object->instances)instances.push_back(id(*i));
                        created.push_back({{"object_id",id(*object)},{"volumes",volumes},{"instance_ids",instances}});
                        mapping.push_back(id(*object));
                    }
                    ctx.invalidate(true);
                    result["removed_object_ids"]=Json::array({object_id});result["removed_volume_ids"]=work->input_volume_ids;result["removed_instance_ids"]=original_instances;result["created_objects"]=created;
                    result["object_mapping"]=Json::array({{{"old_id",object_id},{"new_ids",mapping}}});
                    result["affected_instance_ids"]=original_instances;result["affected_plate_ids"]=affected_plates;result["native_undo"]=true;
                    result["slice_results_invalidated"]=true;
                }
                ctx.finish(std::move(result));
            } catch(const DomainError& e){ctx.fail(e.kind,e.what());}
              catch(const std::exception& e){ctx.fail("geometry_failed",e.what());}
              catch(...){ctx.fail("geometry_failed","Unknown native geometry failure");}
        });
    ensure(queued,"busy","Geometry job could not be queued");
    return nullptr;
}
} // namespace Slic3r::AgentControl
