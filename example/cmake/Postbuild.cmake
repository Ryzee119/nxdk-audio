function(nxdk_add_xiso TARGET_EXE)
    set(TARGET_ISO_DIR "${CMAKE_CURRENT_BINARY_DIR}/${TARGET_EXE}_xiso")
    
    # 1. Convert EXE to XBE
    add_custom_command(
        OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/${TARGET_EXE}.xbe"
        COMMAND ${NXDK_DIR}/tools/cxbe/cxbe -OUT:${CMAKE_CURRENT_BINARY_DIR}/${TARGET_EXE}.xbe -TITLE:${TARGET_EXE} $<TARGET_FILE:${TARGET_EXE}> > /dev/null 2>&1
        DEPENDS ${TARGET_EXE}
        COMMENT "CXBE Conversion: [EXE -> XBE] for ${TARGET_EXE}"
    )

    # 2. Generate ISO
        add_custom_command(
            OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/${TARGET_EXE}.iso"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${TARGET_ISO_DIR}"
            COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_CURRENT_BINARY_DIR}/${TARGET_EXE}.xbe" "${TARGET_ISO_DIR}/default.xbe"
            COMMAND ${NXDK_DIR}/tools/extract-xiso/build/extract-xiso -q -c ${TARGET_ISO_DIR} ${CMAKE_CURRENT_BINARY_DIR}/${TARGET_EXE}.iso > /dev/null 2>&1
            DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/${TARGET_EXE}.xbe"
        COMMENT "XISO Conversion: [XBE -> XISO] for ${TARGET_EXE}"
        )

    # Add a custom target to drive the ISO creation
    add_custom_target(${TARGET_EXE}_iso ALL
        DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/${TARGET_EXE}.iso"
    )
endfunction()

foreach(TARGET_EXE ${EXAMPLES})
    nxdk_add_xiso(${TARGET_EXE})
endforeach()
