# (建议) 首先添加核心库，因为 main 和 test 都依赖它
add_subdirectory(lib)

# 然后添加主程序模块
add_subdirectory(main)

# 最后添加测试模块
add_subdirectory(test)