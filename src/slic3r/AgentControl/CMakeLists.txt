# Keep the extension's source list and feature definition out of upstream's GUI list.
target_sources(libslic3r_gui PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/AgentControl.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Commands.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/RpcServer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Domain.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/FileAccess.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/SceneDomain.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/SettingsDomain.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/GeometryDomain.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/PrintJournal.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/NativeSlicePreview.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/DevicesDomain.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/PrintingDomain.cpp
)
target_compile_definitions(libslic3r_gui PUBLIC SLIC3R_AGENT_CONTROL)

if (WIN32)
    target_link_libraries(libslic3r_gui PRIVATE advapi32 ws2_32 mswsock)
endif ()
