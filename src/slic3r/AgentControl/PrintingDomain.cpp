#include <initializer_list>
#include "libslic3r/CommonDefs.hpp"
#include <system_error>
#include "Domain.hpp"
#include "Hooks.hpp"
#include "PrintJournal.hpp"
#include "PrintMaterials.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintBase.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/DeviceManager.hpp"
#include "slic3r/GUI/DeviceCore/DevManager.h"
#include "slic3r/GUI/DeviceCore/DevDefs.h"
#include "slic3r/GUI/DeviceCore/DevConfigUtil.h"
#include "slic3r/GUI/DeviceCore/DevFilaSystem.h"
#include "slic3r/GUI/DeviceCore/DevFilaBlacklist.h"
#include "slic3r/GUI/DeviceCore/DevFilaSwitch.h"
#include "slic3r/GUI/DeviceCore/DevNozzleSystem.h"
#include "slic3r/GUI/DeviceCore/DevStorage.h"
#include "slic3r/GUI/PartPlate.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/Jobs/Job.hpp"
#include "slic3r/GUI/Jobs/Worker.hpp"
#include "slic3r/Utils/BBLNetworkPlugin.hpp"
#include "slic3r/Utils/BBLPrinterAgent.hpp"
#include "slic3r/Utils/IPrinterAgent.hpp"
#include "slic3r/Utils/NetworkAgent.hpp"
#include "slic3r/Utils/PrintHost.hpp"
#include "slic3r/Utils/Http.hpp"
#include "slic3r/Utils/bambu_networking.hpp"
#include <boost/filesystem/path.hpp>
#include <boost/uuid/random_generator.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <openssl/evp.h>
#include <wx/stdpaths.h>
#include <wx/string.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r::AgentControl {
namespace {
namespace fs=std::filesystem;
using Clock=std::chrono::system_clock;
std::string uuid() { return boost::uuids::to_string(boost::uuids::random_generator()()); }
bool enabled() { const char* value=std::getenv("ORCA_AGENT_CONTROL");return value&&std::string(value)=="1"; }
Json text_schema() { return {{"type","string"},{"minLength",1},{"maxLength",256}}; }
Json choice(std::initializer_list<const char*> values) { Json v=Json::array();for(auto value:values)v.push_back(value);return {{"type","string"},{"enum",v}}; }
std::string digest(const std::string& text) {
    unsigned char bytes[EVP_MAX_MD_SIZE];unsigned size=0;
    ensure(EVP_Digest(text.data(),text.size(),bytes,&size,EVP_sha256(),nullptr)==1,"internal_error","Cannot hash printer identity");
    const char* hex="0123456789abcdef";std::string result;for(unsigned i=0;i<size;++i){result+=hex[bytes[i]>>4];result+=hex[bytes[i]&15];}return result;
}
fs::path private_root(const char* variable,const char* folder) {
    const char* override=std::getenv(variable);
    fs::path path=override&&*override?fs::u8path(override):fs::u8path(wxStandardPaths::Get().GetUserDataDir().ToStdString())/"cache"/folder;
    PrintJournal privacy(path); // Establish current-user-only directory ownership before files exist.
    return path;
}
PrintJournal journal() { return PrintJournal(private_root("ORCA_AGENT_PRINT_JOURNAL","agent-print-intents")); }
std::shared_ptr<IPrinterAgent> active_agent() {
    auto* network=GUI::wxGetApp().getAgent();return network?network->get_printer_agent():std::shared_ptr<IPrinterAgent>{};
}
MachineObject& machine(const std::string& backend,const std::string& wanted) {
    auto agent=active_agent();ensure(agent&&agent->get_agent_info().id==backend,"stale_device","The explicitly selected printer backend is no longer active");
    auto* manager=GUI::wxGetApp().getDeviceManager();ensure(manager,"not_found","Device manager is unavailable");
    auto devices=manager->get_my_machine_list(backend);auto found=devices.find(wanted);
    ensure(found!=devices.end()&&found->second,"not_found","Device ID is not known to this backend");return *found->second;
}
Json device_status(const std::string& backend,MachineObject& device) {
    Json nozzle=Json::array();
    if(device.GetNozzleSystem())for(const auto& entry:device.GetNozzleSystem()->GetExtNozzles())nozzle.push_back({{"extruder_id",entry.first},{"diameter_mm",entry.second.GetNozzleDiameter()}});
    return {{"backend_id",backend},{"device_id",device.get_dev_id()},{"name",device.get_dev_name()},
        {"model_id",device.printer_type},{"connected",device.is_connected()},{"online",device.is_online()},
        {"transport",device.connection_type()},{"print_state",device.print_status},{"task_id",device.task_id_},
        {"subtask_id",device.subtask_id_},{"nozzles",nozzle},
        {"capabilities",{{"lan_submit",backend=="bbl"&&device.is_lan_mode_printer()},{"job_scoped_control",false},
            {"device_scoped_control",backend=="bbl"&&device.is_lan_mode_printer()},{"physical_start_correlation",false}}}};
}
void no_scripts(const DynamicPrintConfig& config) {
    if(auto* scripts=config.option<ConfigOptionStrings>("post_process"))for(const auto& script:scripts->values)
        ensure(script.find_first_not_of(" \t\r\n")==std::string::npos,"scripts_disabled","Configured post-processing scripts require a separately enabled native capability");
}
DynamicPrintConfig full_config() {
    auto* presets=GUI::wxGetApp().preset_bundle;ensure(presets,"not_ready","Preset bundle is unavailable");return presets->full_config(false);
}
std::unique_ptr<PrintHost> configured_host(DynamicPrintConfig& config) {
    auto* address=config.option<ConfigOptionString>("print_host");
    if(!address||address->value.empty())return {};
    return std::unique_ptr<PrintHost>(PrintHost::get_print_host(&config));
}
std::string host_backend(const PrintHost& host) { return "printhost:"+std::string(host.get_name()); }
std::string host_id(const PrintHost& host) { return "host:"+digest(std::string(host.get_name())+"\n"+host.get_host()); }
Json host_summary(const PrintHost& host) {
    return {{"backend_id",host_backend(host)},{"device_id",host_id(host)},{"name",host.get_name()},
        {"capabilities",{{"upload_only",true},{"start_print",false},{"start_unavailable_reason","Backend exposes no verified model/nozzle/material telemetry for this frozen endpoint"},
            {"device_scoped_control",false},{"physical_start_correlation",false}}}};
}
Json warning(const std::string& code,const std::string& message) { return {{"id",digest(code+"\n"+message)},{"code",code},{"message",message}}; }
Json options_schema() {
    Json props;
    for(const char* key:{"bed_leveling","flow_calibration","vibration_calibration","timelapse","use_ams"})props[key]={{"type","boolean"}};
    props["ams_tray_index"]={{"type","integer"},{"minimum",0},{"maximum",253}};
    props["ams_mapping"]={{"type","array"},{"minItems",1},{"maxItems",256},{"items",{
        {"type","object"},{"additionalProperties",false},{"required",Json::array({"filament_id","tray_index"})},
        {"properties",{{"filament_id",{{"type","integer"},{"minimum",1},{"maximum",256}}},
            {"tray_index",{{"type","integer"},{"minimum",0},{"maximum",253}}}}}}}};
    return {{"type","object"},{"additionalProperties",false},{"properties",props}};
}
struct Validation {
    Json snapshot;
    Json warnings=Json::array();
    PrintParams params{};
};
Validation validate_bbl_strict(MachineObject& device,const DynamicPrintConfig& config,GUI::PartPlate& plate,const Json& options,const std::string& action) {
    ensure(device.is_info_ready(),"not_ready","Complete connected device telemetry is required for verified material mapping");
    if(action=="upload_and_start")ensure(!device.is_in_printing()&&!device.is_in_printing_pause()&&!device.is_system_printing()&&device.print_error==0&&!device.upgrade_force_upgrade,"device_busy","Device is printing, paused, upgrading, or reporting an error");
    auto* presets=GUI::wxGetApp().preset_bundle;
    const auto source_model=presets->printers.get_edited_preset().get_printer_type(presets);
    ensure(!source_model.empty()&&!device.printer_type.empty(),"compatibility_unverified","Both source and target printer models must be known");
    ensure(DevPrinterConfigUtil::is_printer_model_compatible(source_model,device),"incompatible_model","Sliced printer model is incompatible with the selected device");
    const auto* diameters=config.option<ConfigOptionFloats>("nozzle_diameter");
    const auto* types=config.option<ConfigOptionStrings>("filament_type");
    ensure(diameters&&diameters->values.size()==1&&types&&!types->values.empty(),"compatibility_unverified","This submission path requires one physical nozzle and known configured filament types");
    ensure(presets->filament_presets.size()==types->values.size(),"compatibility_unverified","Configured filament metadata is inconsistent");
    auto* nozzles=device.GetNozzleSystem();
    ensure(nozzles&&nozzles->GetExtNozzles().size()==1&&nozzles->GetRackNozzles().empty(),"compatibility_unverified","Exactly one installed nozzle without a rack and with reported diameter is required");
    const DevNozzle nozzle=nozzles->GetExtNozzles().begin()->second;
    ensure(device.GetStorage()&&device.GetStorage()->get_sdcard_state()==DevStorage::HAS_SDCARD_NORMAL,"not_ready","A reported writable SD card is required for this LAN destination");
    ensure(nozzle.GetNozzleDiameter()>0&&std::abs(nozzle.GetNozzleDiameter()-diameters->values[0])<1e-5,"incompatible_nozzle","Actual installed nozzle diameter differs from the sliced configuration or is unknown");
    if(const auto* volumes=config.option<ConfigOptionEnumsGeneric>("nozzle_volume_type"))
        ensure(volumes->size()==1&&static_cast<NozzleVolumeType>(volumes->get_at(0))==DevNozzle::ToNozzleVolumeType(nozzle.GetNozzleFlowType()),"incompatible_nozzle","Installed nozzle flow type differs from the sliced configuration");
    const auto* result=plate.get_slice_result();
    ensure(result,"compatibility_unverified","Completed slice material metadata is unavailable");
    const auto used=sliced_filament_indices(result->print_statistics.total_volumes_per_extruder,types->values.size());
    const auto mapping=explicit_ams_mapping(options,used);
    const bool use_ams=options.value("use_ams",false);
    auto system=device.GetFilaSystem();
    if(use_ams)ensure(bool(system),"compatibility_unverified","AMS telemetry is unavailable");
    else ensure(device.vt_slot.size()==1,"compatibility_unverified","Exactly one reported external spool is required");
    const auto indices=use_ams?system->GetTrayIndexMap():std::map<int,DevAmsSlotId>{};
    const auto* vendors=config.option<ConfigOptionStrings>("filament_vendor");
    const auto* colors=config.option<ConfigOptionStrings>("filament_colour");
    DevFilaBlacklist::load_filaments_blacklist_config();
    ensure(DevFilaBlacklist::filaments_blacklist.contains("blacklist"),"compatibility_unverified","Native filament compatibility rules are unavailable");
    Validation validation;
    Json materials=Json::array();
    std::vector<PrintMaterialMapping> native_materials;
    for(const auto index:used) {
        const auto& type=types->values[index];
        ensure(!type.empty(),"compatibility_unverified","A used filament has no configured material type");
        const int required=presets->get_required_hrc_by_filament_type(type);
        ensure(required<=0||(nozzle.GetNozzleType()!=NozzleType::ntUndefine&&Print::get_hrc_by_nozzle_type(nozzle.GetNozzleType())>=required),"incompatible_nozzle","Installed nozzle hardness is insufficient or unverified for a used material");
        const DevAmsTray* tray=nullptr;
        int ams_id=255,slot_id=0,tray_index=255;
        if(use_ams) {
            tray_index=mapping.at(index);
            auto found=indices.find(tray_index);
            ensure(found!=indices.end()&&found->second.first!=255&&found->second.first!=254,"invalid_params","The explicit AMS tray does not exist");
            ams_id=found->second.first;slot_id=found->second.second;
            const auto* ams=system->GetAmsById(std::to_string(ams_id));
            ensure(ams&&ams->GetExtruderId()==nozzle.GetExtruderId(),"compatibility_unverified","Selected AMS is not connected to the verified physical nozzle");
            tray=system->GetAmsTray(std::to_string(ams_id),std::to_string(slot_id));
        } else tray=&device.vt_slot.front();
        ensure(tray&&tray->is_exists&&!tray->is_slot_placeholder&&!tray->m_fila_type.empty(),"compatibility_unverified","Loaded filament telemetry is missing or stale");
        ensure(tray->m_fila_type==type,"incompatible_material","Loaded material type differs from a mapped sliced filament");
        DevFilaBlacklist::CheckFilamentInfo check{};check.dev_id=device.get_dev_id();check.model_id=device.printer_type;
        check.fila_type=type;check.fila_id=tray->setting_id;check.ams_id=ams_id;check.slot_id=slot_id;
        check.fila_name=presets->filament_presets[index];
        if(vendors&&index<vendors->values.size())check.fila_vendor=vendors->values[index];
        check.nozzle_diameter=nozzle.GetNozzleDiameter();check.nozzle_flow=DevNozzle::GetNozzleFlowTypeString(nozzle.GetNozzleFlowType());check.extruder_id=nozzle.GetExtruderId();
        check.has_filament_switch=device.GetFilaSwitch()&&device.GetFilaSwitch()->IsInstalled();
        const auto rules=DevFilaBlacklist::check_filaments_in_blacklist(check);
        ensure(rules.get_items_by_action("prohibition").empty(),"incompatible_material","Native filament rules prohibit a used material/nozzle/AMS combination");
        for(const auto& item:rules.get_items_by_action("warning"))validation.warnings.push_back(warning("filament_rule",item.info_msg.ToStdString()));
        std::string filament_id,source_color;
        if(const auto* preset=presets->filaments.find_preset(presets->filament_presets[index]))filament_id=preset->filament_id;
        if(colors&&index<colors->values.size())source_color=colors->values[index];
        if(source_color.size()==7)source_color+="FF";
        materials.push_back({{"filament_id",index+1},{"material_type",type},{"material_id",tray->setting_id},
            {"filament_preset",presets->filament_presets[index]},{"tray_index",tray_index},{"ams_id",ams_id},{"slot_id",slot_id},
            {"material_tag",tray->tag_uid},{"material_color",tray->color},{"source_color",source_color}});
        // Native single-extruder mode clears the plate filament map and omits
        // nozzleId. A restored map must not label the sole nozzle as left/right.
        native_materials.push_back({index,tray_index,ams_id,slot_id,-1,type,filament_id,source_color,tray->color});
    }
    auto& p=validation.params;p.dev_id=device.get_dev_id();p.dev_ip=device.get_dev_ip();p.password=device.get_access_code();
    p.connection_type="lan";p.use_ssl_for_ftp=device.local_use_ssl_for_ftp;p.use_ssl_for_mqtt=device.local_use_ssl;
    auto agent=active_agent();p.username=agent->default_lan_username();p.ftp_folder=device.get_ftp_folder();p.print_type="from_normal";
    p.task_bed_type=bed_type_to_gcode_string(plate.get_bed_type(true));
    p.task_bed_leveling=options.value("bed_leveling",true);p.task_flow_cali=options.value("flow_calibration",false);
    p.task_vibration_cali=options.value("vibration_calibration",false);p.task_record_timelapse=options.value("timelapse",false);p.task_use_ams=use_ams;
    ensure(!p.task_flow_cali||device.is_support_flow_calibration,"unsupported_option","Device does not advertise flow calibration");
    ensure(!p.task_record_timelapse||device.is_support_timelapse,"unsupported_option","Device does not advertise timelapse");
    ensure(!p.task_bed_leveling||device.is_support_bed_leveling>0,"unsupported_option","Device does not advertise bed leveling");
    {
        const auto native=native_ams_mapping(types->values.size(),native_materials);
        p.ams_mapping=native.at("ams_mapping").dump();
        p.ams_mapping2=native.at("ams_mapping2").dump();
        p.ams_mapping_info=native.at("ams_mapping_info").dump();
    }
    const auto& first=materials.front();
    validation.snapshot={{"compatibility_status","verified"},{"source_model",source_model},{"target_model",device.printer_type},{"nozzle_mm",nozzle.GetNozzleDiameter()},
        {"nozzle_type",static_cast<int>(nozzle.GetNozzleType())},{"nozzle_flow",static_cast<int>(nozzle.GetNozzleFlowType())},
        {"material_type",used.size()==1?first.at("material_type"):Json(nullptr)},
        {"used_filaments",materials},{"multicolor",used.size()>1},{"material_source","completed_native_slice"},
        {"bed_type",p.task_bed_type},{"options",options},{"material_warnings",validation.warnings},
        {"ssl_ftp",p.use_ssl_for_ftp},{"ssl_mqtt",p.use_ssl_for_mqtt},{"private_connection_fingerprint",digest(p.dev_ip+"\n"+p.password+"\n"+p.username)}};
    return validation;
}
// Storage upload does not run the native start preflight. A virtual queue may
// legitimately have no loaded filament/nozzle telemetry; that is an explicit,
// acknowledged uncertainty because the endpoint may still print downstream.
Validation validate_bbl(MachineObject& device,const DynamicPrintConfig& config,GUI::PartPlate& plate,const Json& options,const std::string& action) {
    ensure(device.is_lan_mode_printer()&&device.is_connected(),"not_ready","An active LAN connection to the explicit device is required");
    ensure(!device.get_dev_ip().empty()&&!device.get_access_code().empty(),"not_ready","Native LAN credentials are unavailable");
    try { return validate_bbl_strict(device,config,plate,options,action); }
    catch(const DomainError& issue) {
        if(action!="upload_only")throw;
        const std::set<std::string> compatibility_errors={"compatibility_unverified","incompatible_model","incompatible_nozzle","incompatible_material","unsupported_option","not_ready"};
        if(!compatibility_errors.count(issue.kind))throw;
        Validation result;
        auto& p=result.params;
        p.dev_id=device.get_dev_id();p.dev_ip=device.get_dev_ip();p.password=device.get_access_code();
        p.connection_type="lan";p.username=active_agent()->default_lan_username();p.ftp_folder=device.get_ftp_folder();
        p.use_ssl_for_ftp=device.local_use_ssl_for_ftp;p.use_ssl_for_mqtt=device.local_use_ssl;p.print_type="from_normal";
        p.task_bed_type=bed_type_to_gcode_string(plate.get_bed_type(true));
        Json nozzles=Json::array(),trays=Json::array();
        if(device.GetNozzleSystem())for(const auto& entry:device.GetNozzleSystem()->GetExtNozzles())
            nozzles.push_back({{"id",entry.first},{"diameter",entry.second.GetNozzleDiameter()},{"type",static_cast<int>(entry.second.GetNozzleType())},{"flow",static_cast<int>(entry.second.GetNozzleFlowType())}});
        for(const auto& tray:device.vt_slot)trays.push_back({{"id",tray.id},{"type",tray.m_fila_type},{"material_id",tray.setting_id},{"exists",tray.is_exists}});
        result.snapshot={{"compatibility_status","unverified"},{"compatibility_issue",issue.kind},{"target_model",device.printer_type},
            {"nozzle_mm",nullptr},{"material_type",nullptr},{"bed_type",p.task_bed_type},{"observed_nozzles",nozzles},{"observed_external_trays",trays},{"options",options},
            {"ssl_ftp",p.use_ssl_for_ftp},{"ssl_mqtt",p.use_ssl_for_mqtt},{"private_connection_fingerprint",digest(p.dev_ip+"\n"+p.password+"\n"+p.username)}};
        result.warnings.push_back(warning("upload_compatibility_"+issue.kind,std::string(issue.what())+". Storage upload does not verify a safe downstream print; the endpoint may queue or start it."));
        return result;
    }
}
struct Plan {
    std::string id,backend,device,plate,state,action,mode,artifact_hash,machine_token,source_hash;
    Json target,summary,warnings,options;
    std::uint64_t epoch=0,library_generation=0;
    unsigned slice_id=0;
    Clock::time_point expires;
    fs::path directory,artifact;
    std::uintmax_t size=0;
    std::shared_ptr<IPrinterAgent> agent;
    std::unique_ptr<PrintHost> host;
    PrintParams params{};
    bool consumed=false;
    ~Plan(){std::error_code ignored;if(!directory.empty())fs::remove_all(directory,ignored);}
};
std::map<std::string,std::shared_ptr<Plan>>& plans() { static std::map<std::string,std::shared_ptr<Plan>> data;return data; }
void expire_plans() { auto& data=plans();for(auto it=data.begin();it!=data.end();)if(it->second->expires<Clock::now())it=data.erase(it);else ++it; }
Json journal_status(const std::string& operation) {
    Json state=journal().read(operation);ensure(!state.is_null(),"not_found","Print intent is not present in the durable journal");return {{"operation_id",operation},{"journal",state}};
}
Json prepare(DomainContext& ctx,const Json& params) {
    expire_plans();ensure(plans().size()<64,"busy","Print plan cache is full; let existing plans expire");
    ensure(!ctx.plater.only_gcode_mode(),"compatibility_unverified","Prepare requires a native model slice, not an imported G-code preview");
    auto& plate=resolve_plate(ctx,params);ensure(plate.has_printable_instances()&&plate.is_slice_result_ready_for_print()&&plate.is_valid_gcode_file(),"not_ready","Explicit plate must have a valid completed native slice");
    auto config=full_config();config.apply(*plate.config());no_scripts(config);
    PrintBase* print=nullptr;plate.get_print(&print,nullptr,nullptr);ensure(print,"not_ready","Plate print is unavailable");no_scripts(print->full_print_config());
    auto plan=std::make_shared<Plan>();plan->id=uuid();plan->backend=params.at("backend_id");plan->device=params.at("device_id");
    plan->plate=params.at("plate_id");plan->state=ctx.state_token;plan->epoch=ctx.document_epoch;plan->target=ctx.target;
    plan->action=params.at("action");plan->mode=params.at("endpoint_mode");plan->options=params.value("options",Json::object());plan->expires=Clock::now()+std::chrono::seconds(600);
    ensure(plate.get_slice_result(),"not_ready","Completed native slice metadata is unavailable");
    plan->slice_id=plate.get_slice_result()->id;plan->source_hash=file_sha256(fs::u8path(plate.get_tmp_gcode_path()));
    plan->warnings=Json::array();
    if(auto* result=plate.get_slice_result())for(const auto& item:result->warnings) {
        // These native numbers are notification categories, not a monotonic
        // severity ordering (bed-temperature and HRC cautions use level 3).
        // The ready-for-print gate above owns toolpath/G-code/material errors.
        if(item.level>0)plan->warnings.push_back(warning(item.error_code,item.msg));
    }
    if(plan->mode=="unknown")plan->warnings.push_back(warning("routing_unknown","Endpoint downstream routing and possible automatic printing are unknown, including for upload-only"));
    if(plan->mode=="queue")plan->warnings.push_back(warning("endpoint_queue","A successful upload does not confirm that the endpoint added the file to its queue. The endpoint may enqueue or automatically print uploaded files."));
    plan->directory=private_root("ORCA_AGENT_PRINT_ARTIFACTS","agent-print-artifacts")/plan->id;
    PrintJournal privacy(plan->directory);
    if(plan->backend=="bbl") {
        for(const char* key:{"bed_leveling","flow_calibration","vibration_calibration","timelapse","use_ams"})
            if(!plan->options.contains(key))plan->options[key]=std::string(key)=="bed_leveling";
        if(plan->action=="upload_only"&&plan->options.value("use_ams",false))
            plan->warnings.push_back(warning("upload_ams_mapping_not_applied","Upload-only does not send a print-start command with the selected AMS mapping. The endpoint may select different slots downstream."));
        auto& device=machine(plan->backend,plan->device);auto validation=validate_bbl(device,config,plate,plan->options,plan->action);
        plan->agent=active_agent();ensure(bool(std::dynamic_pointer_cast<BBLPrinterAgent>(plan->agent)),"unsupported_backend","Only the native BBL agent implements this submission capability");plan->library_generation=BBLNetworkPlugin::instance().operation_generation();plan->params=validation.params;
        plan->machine_token=digest(validation.snapshot.dump());
        for(const auto& item:validation.warnings)plan->warnings.push_back(item);
        plan->artifact=plan->directory/(plan->id+".3mf");
        const int index=plate.get_index();
        ensure(ctx.plater.agent_export_print_artifact(boost::filesystem::path(plan->artifact.u8string()),index)==0,"export_failed","Native print artifact export failed");
        plan->params.plate_index=index+1;plan->params.filename=plan->artifact.u8string();
        // Match the native SendJob contract: receivers such as Bambuddy use the
        // uploaded filename's .3mf suffix to decide whether to enqueue it.
        plan->params.project_name="Agent-"+plan->id+(plan->action=="upload_only"?".gcode.3mf":"");
        plan->params.task_name=plan->id;
        plan->summary={{"status",validation.snapshot["compatibility_status"]},{"model",validation.snapshot["target_model"]},{"nozzle_mm",validation.snapshot["nozzle_mm"]},{"material",validation.snapshot["material_type"]},{"used_filaments",validation.snapshot.value("used_filaments",Json::array())},{"multicolor",validation.snapshot.value("multicolor",false)},{"bed_type",validation.snapshot["bed_type"]}};
    } else {
        plan->host=configured_host(config);ensure(plan->host&&host_backend(*plan->host)==plan->backend&&host_id(*plan->host)==plan->device,"stale_device","Explicit host does not match the configured native endpoint");
        ensure(plan->action=="upload_only","compatibility_unverified","Generic PrintHost start lacks verified model/nozzle/material telemetry; only explicit upload is available");
        ensure(plan->options.empty(),"unsupported_option","Calibration and AMS options are BBL capabilities");
        plan->artifact=plan->directory/(plan->id+".gcode");
        const fs::path source=fs::u8path(plate.get_tmp_gcode_path());
        ensure(fs::is_regular_file(fs::symlink_status(source)),"invalid_artifact","Sliced G-code is not a regular local file");
        ensure(fs::copy_file(source,plan->artifact,fs::copy_options::none),"export_failed","Cannot freeze the sliced artifact");
        plan->summary={{"model_compatibility","unverified"},{"nozzle_compatibility","unverified"},{"material_compatibility","unverified"}};
    }
    ensure(ctx.current_state()==ctx.state_token,"conflict","Document changed during print preparation");
    plan->size=fs::file_size(plan->artifact);ensure(plan->size>0,"invalid_artifact","Prepared artifact is empty");plan->artifact_hash=file_sha256(plan->artifact);
    fs::permissions(plan->artifact,fs::perms::owner_read,fs::perm_options::replace);
    const auto report=print_report(ctx,plate);
    plans().emplace(plan->id,plan);
    return {{"plan_id",plan->id},{"operation_id",plan->id},{"backend_id",plan->backend},{"device_id",plan->device},{"plate_id",plan->plate},
        {"action",plan->action},{"endpoint_mode",plan->mode},{"slice_id",plan->slice_id},{"source_gcode_sha256",plan->source_hash},{"expires_at",std::chrono::duration_cast<std::chrono::seconds>(plan->expires.time_since_epoch()).count()},
        {"artifact",{{"sha256",plan->artifact_hash},{"bytes",plan->size},{"format",plan->artifact.extension().string()}}},
        {"compatibility",plan->summary},{"report",report},{"warnings",plan->warnings},{"options",plan->options},{"start_options_applied",plan->action=="upload_and_start"},{"physical_start_confirmed",false}};
}
struct Submission {
    Json outcome;
    std::string receipt;
    std::atomic<bool> cancelled{false};
    std::shared_ptr<void> lease;
};
Json submit(DomainContext& ctx,const Json& params) {
    ensure(enabled(),"printing_disabled","Owner must enable ORCA_AGENT_CONTROL=1 for transmission and device controls");
    expire_plans();const std::string wanted=params.at("plan_id");auto found=plans().find(wanted);
    if(found==plans().end()) {
        Json previous=journal().read(wanted);
        ensure(!previous.is_null(),"plan_expired","Print plan is absent or expired; prepare a new plan explicitly");
        return {{"operation_id",wanted},{"already_consumed",true},{"journal",previous}};
    }
    auto plan=found->second;
    ensure(plan->target==ctx.target&&plan->epoch==ctx.document_epoch&&plan->state==ctx.state_token,"stale_plan","Print plan belongs to another window or document state");
    if(plan->consumed)return journal_status(plan->id);
    const Json accepted=params.value("accepted_warning_ids",Json::array());
    std::set<std::string> required_ids,accepted_ids;
    for(const auto& item:plan->warnings)required_ids.insert(item.at("id").get<std::string>());
    for(const auto& item:accepted)accepted_ids.insert(item.get<std::string>());
    ensure(required_ids==accepted_ids,"unresolved_warnings","Submit requires the exact warning IDs returned by this plan");
    auto& plate=resolve_plate(ctx,Json{{"plate_id",plan->plate}});
    ensure(plate.is_slice_result_ready_for_print()&&plate.is_valid_gcode_file()&&plate.get_slice_result()&&plate.get_slice_result()->id==plan->slice_id&&file_sha256(fs::u8path(plate.get_tmp_gcode_path()))==plan->source_hash,"stale_plan","The prepared plate slice identity or G-code content changed");
    auto config=full_config();config.apply(*plate.config());no_scripts(config);
    PrintBase* print=nullptr;plate.get_print(&print,nullptr,nullptr);ensure(print,"not_ready","Plate print is unavailable");no_scripts(print->full_print_config());
    auto state=std::make_shared<Submission>();
    if(plan->backend=="bbl") {
        ensure(active_agent()==plan->agent&&BBLNetworkPlugin::instance().operation_generation()==plan->library_generation,"stale_device","The prepared printer backend was switched or reloaded");
        auto& device=machine(plan->backend,plan->device);
        const auto validation=validate_bbl(device,config,plate,plan->options,plan->action);
        ensure(digest(validation.snapshot.dump())==plan->machine_token,"stale_plan","Device model, nozzle, loaded material, mapping or LAN credentials changed");
        state->lease=BBLNetworkPlugin::instance().retain_for_operation();ensure(bool(state->lease),"backend_unavailable","Native BBL library is not loaded");
        auto& library=BBLNetworkPlugin::instance();
        ensure(plan->action=="upload_only"?library.get_start_send_gcode_to_sdcard()!=nullptr:library.get_start_local_print()!=nullptr,"backend_unavailable","Loaded library does not implement the requested LAN operation");
    }
    ensure(fs::is_regular_file(fs::symlink_status(plan->artifact))&&fs::file_size(plan->artifact)==plan->size&&file_sha256(plan->artifact)==plan->artifact_hash,"artifact_changed","Frozen print artifact changed");
    ctx.register_cancel([state](){state->cancelled.store(true,std::memory_order_relaxed);});
    const fs::path journal_directory=private_root("ORCA_AGENT_PRINT_JOURNAL","agent-print-intents");
    const bool queued=GUI::queue_job(ctx.plater.get_ui_job_worker(),
        [plan,state,journal_directory](GUI::Job::Ctl& ctl) {
            const auto cancelled=[&](){return state->cancelled.load(std::memory_order_relaxed)||ctl.was_canceled();};
            if(cancelled())throw DomainError("cancelled","Submission cancelled before consuming its intent");
            ensure(fs::is_regular_file(fs::symlink_status(plan->artifact))&&fs::file_size(plan->artifact)==plan->size&&file_sha256(plan->artifact)==plan->artifact_hash,"artifact_changed","Artifact changed before transmission");
            Json intent={{"operation_id",plan->id},{"backend_id",plan->backend},{"device_id",plan->device},
                {"target",plan->target},{"document_epoch",plan->epoch},{"state_token",plan->state},{"plate_id",plan->plate},
                {"action",plan->action},{"endpoint_mode",plan->mode},{"slice_id",plan->slice_id},{"source_gcode_sha256",plan->source_hash},{"artifact_sha256",plan->artifact_hash},{"artifact_bytes",plan->size}};
            PrintJournal log(journal_directory);
            if(!log.claim(plan->id,intent)) {
                const auto previous=log.read(plan->id);
                ensure(previous["intent"]==intent,"request_conflict","Operation ID belongs to a different consumed intent");
                state->outcome={{"state","already_consumed"},{"journal",previous}};return;
            }
            state->outcome={{"state","outcome_unknown"},{"physical_start_confirmed",false}};
            // From this point every error/cancel is potentially post-send. The
            // durable claim remains consumed, including a negative backend code.
            try {
                if(cancelled()){state->outcome["reason"]="Cancelled after claim; no automatic retry";log.record_outcome(plan->id,state->outcome);return;}
                int code=-1;bool success=false;
                if(plan->backend=="bbl") {
                    auto update=[&ctl](int stage,int percentage,std::string) {
                        const int progress=stage==PrintingStageUpload?std::clamp(percentage,0,99):stage==PrintingStageFinished?100:0;
                        ctl.update_status(progress,"Native LAN transfer");
                    };
                    auto wait=[state](int,std::string info) {
                        const auto receipt=Json::parse(info,nullptr,false);
                        if(receipt.is_object()&&receipt.contains("job_id")) {
                            const auto& value=receipt["job_id"];
                            if(value.is_string())state->receipt=value.get<std::string>();
                            else if(value.is_number_integer())state->receipt=value.dump();
                        }
                        // We do not wait on mutable GUI MachineObject pointers or
                        // treat another active task / a timeout as our completion.
                        return true;
                    };
                    if(plan->action=="upload_only")code=plan->agent->start_send_gcode_to_sdcard(plan->params,update,cancelled,wait);
                    else code=plan->agent->start_local_print(plan->params,update,cancelled);
                    success=code==BAMBU_NETWORK_SUCCESS;
                } else {
                    PrintHostUpload upload;upload.source_path=boost::filesystem::path(plan->artifact.u8string());
                    upload.upload_path=boost::filesystem::path(plan->id+".gcode");upload.post_action=PrintHostPostUploadAction::None;
                    success=plan->host->upload(std::move(upload),[&](Http::Progress progress,bool& cancel){cancel=cancelled();ctl.update_status(progress.ultotal?static_cast<int>(100.0*progress.ulnow/progress.ultotal):0,"Native host transfer");},
                        [](wxString){},[](wxString,wxString){});
                    code=success?0:-1;
                }
                state->outcome["native_code"]=code;
                state->outcome["state"]=success?(plan->action=="upload_only"?"uploaded":"submission_accepted"):"outcome_unknown";
                state->outcome["cancel_requested"]=cancelled();
                state->outcome["remote_job_id"]=state->receipt.empty()?Json(nullptr):Json(state->receipt);
                state->outcome["endpoint_mode"]=plan->mode;
                state->outcome["downstream_printing"]="unverified";
                if(plan->mode=="queue")state->outcome["queue_state"]="unverified";
                if(!success)state->outcome["reason"]="Native call failed after possible transmission; retry is forbidden";
                log.record_outcome(plan->id,state->outcome);
            } catch(...) {
                state->outcome["state"]="outcome_unknown";
                state->outcome["reason"]="Transfer or durable outcome recording failed; intent remains consumed and must be reconciled";
                try {log.record_outcome(plan->id,state->outcome);}catch(...){}
            }
        },
        [plan,state,context=ctx](bool,std::exception_ptr& exception) mutable {
            auto lifetime=context.lifetime.lock();if(!lifetime){exception=nullptr;return;}
            try {
                if(exception){auto error=exception;exception=nullptr;std::rethrow_exception(error);}
                context.finish({{"operation_id",plan->id},{"outcome",state->outcome},{"physical_start_confirmed",false}});
            }catch(const DomainError& e){context.fail(e.kind,e.what());}
             catch(...){context.fail("submission_failed","Native submission failed before a durable intent was consumed");}
            state->lease.reset();
        });
    ensure(queued,"busy","Native submission worker could not accept the job");
    plan->consumed=true;
    return nullptr;
}
Json control(DomainContext& ctx,const Json& params) {
    ensure(enabled(),"printing_disabled","Owner must enable ORCA_AGENT_CONTROL=1 for device controls");
    const std::string backend=params.at("backend_id"),device=params.at("device_id"),action=params.at("action"),operation=params.at("operation_id");
    ensure(backend=="bbl","unsupported_backend","Device-wide controls require a captured native BBL LAN backend");
    auto& target=machine(backend,device);ensure(target.is_connected()&&target.is_lan_mode_printer(),"not_ready","Explicit device has no connected LAN channel");
    if(action=="pause")ensure(target.can_pause(),"not_ready","Explicit device is not in a pausable state");
    if(action=="resume") {
        ensure(target.can_resume(),"not_ready","Explicit device is not paused");
        // Mirror the native resume boundary without opening its modal dialog.
        ensure(target.jobState_<=1,"requires_device_action","This processing task can only be resumed on the printer");
    }
    const std::string native_sequence=std::to_string(MachineObject::m_sequence_id++);
    const std::string native_task=target.task_id_;
    auto agent=active_agent();ensure(bool(std::dynamic_pointer_cast<BBLPrinterAgent>(agent)),"unsupported_backend","Only the native BBL agent implements device controls");auto state=std::make_shared<Submission>();
    state->lease=BBLNetworkPlugin::instance().retain_for_operation();ensure(bool(state->lease)&&BBLNetworkPlugin::instance().get_send_message_to_printer(),"backend_unavailable","Native LAN control is unavailable");
    ctx.register_cancel([state](){state->cancelled.store(true,std::memory_order_relaxed);});
    const fs::path journal_directory=private_root("ORCA_AGENT_PRINT_JOURNAL","agent-print-intents");
    const bool queued=GUI::queue_job(ctx.plater.get_ui_job_worker(),
        [agent,state,backend,device,action,operation,native_sequence,native_task,journal_directory](GUI::Job::Ctl& ctl){
            if(state->cancelled.load()||ctl.was_canceled())throw DomainError("cancelled","Device control cancelled before transmission");
            PrintJournal log(journal_directory);Json intent={{"operation_id",operation},{"backend_id",backend},{"device_id",device},{"action",action},{"scope","device"}};
            if(!log.claim(operation,intent)) {
                const auto previous=log.read(operation);
                ensure(previous["intent"]==intent,"request_conflict","Operation ID belongs to another durable intent");state->outcome=previous;return;
            }
            Json command={{"print",{{"command",action},{"param",""},{"sequence_id",native_sequence}}}};
            if(action=="stop")command["print"]["job_id"]=native_task;
            int code=-1;try{code=agent->send_message_to_printer(device,command.dump(),1,0);}catch(...){}
            state->outcome={{"state",code==0?"command_accepted":"outcome_unknown"},{"native_code",code},{"scope","device"},{"action",action},{"physical_effect_confirmed",false}};
            log.record_outcome(operation,state->outcome);
        },
        [state,operation,context=ctx](bool,std::exception_ptr& exception)mutable{
            if(context.lifetime.expired()){exception=nullptr;return;}
            try{if(exception){auto error=exception;exception=nullptr;std::rethrow_exception(error);}context.finish({{"operation_id",operation},{"outcome",state->outcome}});}
            catch(const DomainError& e){context.fail(e.kind,e.what());}catch(...){context.fail("outcome_unknown","Device command outcome is unknown; inspect its durable intent before any retry");}
            state->lease.reset();
        });
    ensure(queued,"busy","Native device control worker could not accept the job");return nullptr;
}
} // namespace

CommandSpecs printing_commands() {
    const Json identity={{"backend_id",text_schema()},{"device_id",text_schema()}};
    CommandSpecs specs;
    specs["printers.list"]=command_spec(false,Json::object());
    specs["printers.describe"]=command_spec(false,identity,{"backend_id","device_id"});
    specs["printers.status"]=command_spec(false,identity,{"backend_id","device_id"});
    Json prepare_props=identity;prepare_props["plate_id"]=text_schema();prepare_props["action"]=choice({"upload_only","upload_and_start"});
    prepare_props["endpoint_mode"]=choice({"direct","queue","unknown"});prepare_props["options"]=options_schema();
    specs["printing.prepare"]=command_spec(true,prepare_props,{"backend_id","device_id","plate_id","action","endpoint_mode"});
    specs["printing.submit"]=command_spec(true,{{"plan_id",text_schema()},{"accepted_warning_ids",{{"type","array"},{"maxItems",128},{"uniqueItems",true},{"items",text_schema()}}}},{"plan_id"});
    specs["printing.status"]=command_spec(false,{{"operation_id",text_schema()}},{"operation_id"});
    specs["printing.reconcile"]=command_spec(false,{{"operation_id",text_schema()}},{"operation_id"});
    Json ctrl=identity;ctrl["action"]=choice({"pause","resume","stop"});ctrl["scope"]=choice({"device"});
    ctrl["operation_id"]={{"type","string"},{"minLength",36},{"maxLength",36},{"pattern","^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$"}};
    specs["printers.control"]=command_spec(true,ctrl,{"backend_id","device_id","action","scope","operation_id"});
    specs["printing.prepare"].schema["description"]="Prepare an immutable ten-minute plan without contacting the printer. BBL LAN start requires actual single-nozzle telemetry and native material/AMS rules for every used sliced filament; storage upload exposes incompatible or missing telemetry as explicit acknowledgement warnings. Generic PrintHost supports explicit upload only, with compatibility unverified. Endpoint queue or unknown routing can start downstream printing even for upload-only; an upload success does not verify queue insertion.";
    specs["printing.submit"].schema["description"]="Consume a durable single-use print intent before one native transmission. Requires owner opt-in and exact plan warnings. No cloud fallback or automatic retry; acceptance/upload does not prove queue insertion or physical printing.";
    specs["printers.control"].schema["description"]="Explicit device-wide BBL LAN pause/resume/stop, affecting whichever job the endpoint currently runs. This does not provide atomic job-scoped control. A durable operation UUID prevents retransmission.";
    return specs;
}
Json execute_printing(DomainContext& context,const std::string& method,const Json& params) {
    auto specs=printing_commands();auto spec=specs.find(method);ensure(spec!=specs.end(),"unknown_method","Unknown printing command");validate_parameters(params,spec->second);
    if(method=="printing.prepare")return prepare(context,params);
    if(method=="printing.submit")return submit(context,params);
    if(method=="printers.control")return control(context,params);
    if(method=="printing.status")return journal_status(params.at("operation_id"));
    if(method=="printing.reconcile") {
        const std::string operation=params.at("operation_id");Json state=journal_status(operation);auto& record=state["journal"];
        const auto& intent=record.at("intent");const auto& outcome=record.at("outcome");
        state["physical_start_confirmed"]=false;
        state["correlation"]="unavailable";
        if(intent.value("backend_id","")=="bbl") {
            auto& device=machine("bbl",intent.at("device_id"));state["endpoint_status"]=device_status("bbl",device);
            const auto receipt=outcome.value("remote_job_id",Json());
            if(receipt.is_string()&&!receipt.get<std::string>().empty()) {
                const std::string remote=receipt.get<std::string>();
                const bool same=remote==device.task_id_||remote==device.subtask_id_;
                state["correlation"]=same?"exact_remote_job_id":"different_or_unobserved_job";
                state["endpoint_printing_confirmed"]=same&&device.is_in_printing();
            }
        }
        return state;
    }
    if(method=="printers.list") {
        Json entries=Json::array();auto agent=active_agent();auto* manager=GUI::wxGetApp().getDeviceManager();
        if(agent&&manager)for(const auto& entry:manager->get_my_machine_list(agent->get_agent_info().id))if(entry.second)entries.push_back(device_status(agent->get_agent_info().id,*entry.second));
        auto config=full_config();if(auto host=configured_host(config))entries.push_back(host_summary(*host));
        return {{"printers",entries},{"transmission_enabled",enabled()}};
    }
    const std::string backend=params.at("backend_id"),device=params.at("device_id");
    if(backend.rfind("printhost:",0)==0) {
        auto config=full_config();auto host=configured_host(config);ensure(host&&host_backend(*host)==backend&&host_id(*host)==device,"stale_device","Configured host identity changed");
        Json result=host_summary(*host);result["remote_state"]="unavailable";return result;
    }
    return device_status(backend,machine(backend,device));
}
} // namespace Slic3r::AgentControl
