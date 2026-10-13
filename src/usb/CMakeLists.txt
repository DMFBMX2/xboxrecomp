# USB: MCPX OHCI host controllers, so a title's own XAPI can find a pad

add_library(xbox_usb STATIC
    ohci.c
    usb_gamepad.c
)

target_include_directories(xbox_usb PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(xbox_usb PUBLIC platform xbox_input)

if(MSVC)
    target_compile_options(xbox_usb PRIVATE /W4 /O2
        /wd4210  # extern function declared inside the one function that calls it: meant
    )
    # The standard C library is used as it is (getenv); no _s variants wanted.
    target_compile_definitions(xbox_usb PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(xbox_usb PRIVATE -Wall -Wextra -O2)
endif()
