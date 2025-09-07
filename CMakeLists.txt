cmake_minimum_required(VERSION 3.21)

project(RTOSDemo LANGUAGES C ASM)

set(FREERTOS_BASE "${CMAKE_CURRENT_LIST_DIR}/")
set(FREERTOS_PORT GCC_ARM_CM3)
set(FREERTOS_HEAP 4)
add_library(freertos_config INTERFACE)
target_include_directories(freertos_config INTERFACE .)
add_subdirectory("${FREERTOS_BASE}/Source" FreeRTOS_kernel)

add_executable(RTOSDemo
    startup.c
    main.c
    LocalDemoFiles/osram128x64x4.c
    driver/ustdlib.c
    syscalls.c
    isr_weak.c
)
target_include_directories(RTOSDemo PUBLIC
    .
    LocalDemoFiles
    include
    driver
    Source
)
target_link_libraries(RTOSDemo PUBLIC
    freertos_kernel
    "${CMAKE_CURRENT_LIST_DIR}/driver/arm-none-eabi-gcc/libdriver.a"
    "${CMAKE_CURRENT_LIST_DIR}/driver/arm-none-eabi-gcc/libgr.a"
)

add_custom_target(run
    COMMAND qemu-system-arm
        -machine lm3s6965evb
        -monitor null
        -semihosting
        --semihosting-config enable=on,target=native
        -serial stdio
        -nographic
        -s # gdb tcp::1234
        -S
        -kernel $<TARGET_FILE:RTOSDemo>
    VERBATIM COMMAND_EXPAND_LISTS
    COMMENT "You can now attach GDB to localhost:1234"
)
