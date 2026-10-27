# 86Box-Next: the USB controllers against fake guest memory and fake devices,
# and the host passthrough against a fake libusb.  No test framework; each
# test is a program that returns non-zero on failure.
if(BUILD_TESTING)
    # The card list (usb_uhci.c) has the OHCI card, so every test with the
    # UHCI links the OHCI too.
    set(USB_OHCI_SOURCES
        ../../src/usb/usb_ohci.c
        ../../src/usb/usb_chipset.c
        ../../src/usb/usb_opti861.c
    )

    # Each controller test is built once per card: VIA, and Intel (-D*_INTEL).
    foreach(t uhci ehci)
        foreach(card via intel)
            set(name usb_${t}_${card}_tests)
            add_executable(${name}
                usb_${t}_test.c
                usb_test_fakes.c
                ../../src/usb/usb_uhci.c
                ../../src/usb/usb_bus.c
                ${USB_OHCI_SOURCES}
                $<$<STREQUAL:${t},ehci>:../../src/usb/usb_ehci.c>
            )
            target_include_directories(${name} PRIVATE
                ${PROJECT_SOURCE_DIR}/src/include
                ${PROJECT_BINARY_DIR}/src/include
            )
            if(card STREQUAL "intel")
                string(TOUPPER "${t}_INTEL" define)
                target_compile_definitions(${name} PRIVATE ${define})
            endif()
            add_test(NAME Usb.${t}.${card} COMMAND ${name})
        endforeach()
    endforeach()

    # The PIIX3's USB function: configuration space and legacy support.
    add_executable(usb_piix3_tests
        usb_piix3_test.c
        usb_test_fakes.c
        ../../src/usb/usb_uhci.c
        ../../src/usb/usb_bus.c
        ${USB_OHCI_SOURCES}
    )
    target_include_directories(usb_piix3_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/src/include
        ${PROJECT_BINARY_DIR}/src/include
    )
    add_test(NAME Usb.piix3 COMMAND usb_piix3_tests)

    # The OHCI, on the OPTi FireLink card.
    add_executable(usb_ohci_tests
        usb_ohci_test.c
        usb_test_fakes.c
        ../../src/usb/usb_uhci.c
        ../../src/usb/usb_bus.c
        ${USB_OHCI_SOURCES}
    )
    target_include_directories(usb_ohci_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/src/include
        ${PROJECT_BINARY_DIR}/src/include
    )
    add_test(NAME Usb.ohci COMMAND usb_ohci_tests)

    # The southbridges' USB function behind upstream's usb.h.
    add_executable(usb_chipset_tests
        usb_chipset_test.c
        usb_test_fakes.c
        ../../src/usb/usb_uhci.c
        ../../src/usb/usb_bus.c
        ${USB_OHCI_SOURCES}
    )
    target_include_directories(usb_chipset_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/src/include
        ${PROJECT_BINARY_DIR}/src/include
    )
    add_test(NAME Usb.chipset COMMAND usb_chipset_tests)

    # Upstream's hub on our bus, through the bridge.
    add_executable(usb_hub_tests
        usb_hub_test.c
        usb_test_fakes.c
        ../../src/usb/usb_uhci.c
        ../../src/usb/usb_bus.c
        ../../src/usb/usb_devbridge.c
        ../../src/usb_dev.c
        ../../src/usb_hub.c
        ${USB_OHCI_SOURCES}
    )
    target_include_directories(usb_hub_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/src/include
        ${PROJECT_BINARY_DIR}/src/include
    )
    add_test(NAME Usb.hub COMMAND usb_hub_tests)

    # The USB 2.0 hub and the EHCI's split transactions.
    add_executable(usb_hub2_tests
        usb_hub2_test.c
        usb_test_fakes.c
        ../../src/usb/usb_uhci.c
        ../../src/usb/usb_ehci.c
        ../../src/usb/usb_bus.c
        ../../src/usb/usb_devbridge.c
        ../../src/usb/usb_hub2.c
        ../../src/usb_dev.c
        ../../src/usb_hub.c
        ${USB_OHCI_SOURCES}
    )
    target_include_directories(usb_hub2_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/src/include
        ${PROJECT_BINARY_DIR}/src/include
    )
    add_test(NAME Usb.hub2 COMMAND usb_hub2_tests)

    # The emulated keyboard and mouse, driven packet by packet.
    add_executable(usb_hid_tests
        usb_hid_test.c
        usb_test_fakes.c
        ../../src/usb/usb_bus.c
        ../../src/usb/usb_hid.c
    )
    target_include_directories(usb_hid_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/src/include
        ${PROJECT_BINARY_DIR}/src/include
    )
    add_test(NAME Usb.hid COMMAND usb_hid_tests)

    find_package(PkgConfig QUIET)
    if(PKG_CONFIG_FOUND)
        pkg_check_modules(LIBUSB_TEST QUIET libusb-1.0)
    endif()
    if(LIBUSB_TEST_FOUND)
        # libusb's header only: every function the backend calls is faked.
        add_executable(usb_host_tests
            usb_host_test.c
            ../../src/usb/usb_host.c
            ../../src/usb/usb_speed.c
        )
        target_compile_definitions(usb_host_tests PRIVATE USE_LIBUSB)
        target_include_directories(usb_host_tests PRIVATE
            ${PROJECT_SOURCE_DIR}/src/include
            ${PROJECT_BINARY_DIR}/src/include
            ${LIBUSB_TEST_INCLUDE_DIRS}
        )
        if(WIN32)
            target_link_libraries(usb_host_tests PRIVATE setupapi)
        endif()
        add_test(NAME Usb.host COMMAND usb_host_tests)
    endif()
endif()
