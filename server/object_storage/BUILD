cc_library(
    name = "object_storage",
    srcs = [
        "src/ftp_command_handlers.cpp",
        "src/http_method_handlers.cpp",
        "src/object_storage.cpp",
        "src/session.cpp",
    ],
    hdrs = [
        "src/object_storage.hpp",
        "src/session.hpp",
    ],
    visibility = [
        "//:__pkg__",
        "//test:__subpackages__",
    ],
    deps = [
        "//filesystem/memory_fs",
        "//protocol/detector:protocol_detector",
        "//protocol/ftp/request:ftp_parser",
        "//protocol/ftp/response:ftp_response",
        "//protocol/http/request:http_parser",
        "//protocol/http/response:http_response",
        "//server:server_interface",
        "//user/database:user_database",
        "@boost//:asio",
        "@boost//:log",
    ],
)

cc_test(
    name = "object_storage_test",
    srcs = ["test/object_storage_test.cpp"],
    deps = [
        ":object_storage",
        "@googletest//:gtest_main",
    ],
)
