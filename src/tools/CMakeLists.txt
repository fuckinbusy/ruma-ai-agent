# tools: the agent's tools, a static set. Changing calls only through tools::os (fenced in tests),
# reading and changing only with a security::Permit. Never linked into wa_ui.
add_library(wa_tools STATIC
    os.cpp
    recycle.cpp
    process_os.cpp
    shell.cpp
    trash.cpp
    all.cpp
    call.cpp
    guarded_fs.cpp
    fs_read.cpp
    glob.cpp
    shell_jobs.cpp
    edit_text.cpp
    fs_edit.cpp
    fs_write.cpp
    system.cpp
    image.cpp
    image_tools.cpp
    ocr.cpp
    capture.cpp
    doc_tools.cpp
    docs/zip.cpp
    docs/xml.cpp
    docs/ooxml_read.cpp
    docs/pdf.cpp
    docs/csv.cpp
    docs/ooxml_write.cpp
    winsys/registry.cpp
    winsys/startup.cpp
    winsys/services.cpp
    winsys/tasks.cpp
    winsys/wmi.cpp
    winsys/signature.cpp
    winsys_tools.cpp
    winsys_change_tools.cpp
    winsys/change.cpp
    winsys/disk.cpp
    winsys/threats.cpp
    winsys/check.cpp
    winsys_disk_tools.cpp
    winsys_uninstall_tools.cpp
    winsys_check_tools.cpp
    winsys_quarantine_tools.cpp
    winsys/sample.cpp
    winsys/net.cpp
    winsys/events.cpp
    winsys/apps.cpp
    winsys/defender.cpp
    winsys/private_rules.cpp
    winsys/sensitive.cpp
    winsys/look.cpp
    winsys/reader_link.cpp
)
find_package(glaze CONFIG REQUIRED)
target_include_directories(wa_tools PUBLIC ${PROJECT_SOURCE_DIR}/src)
find_package(miniz CONFIG REQUIRED) # DOCX, XLSX and PPTX are ZIP files (phase 9)
target_link_libraries(wa_tools PUBLIC wa_security wa_core PRIVATE glaze::glaze miniz::miniz wa_markdown)
wa_target_defaults(wa_tools)
