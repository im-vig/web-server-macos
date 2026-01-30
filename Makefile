# 编译器
CXX = g++

# 编译选项
# -std=c++11: 使用 C++11 标准
# -O3: 最高级优化，提升服务器运行速度
# -Wall: 显示所有警告信息
# -g: 添加调试信息（方便使用 gdb 调试）
CXXFLAGS = -std=c++11 -O3 -Wall -g

# 目标程序名
TARGET = server

# 源文件 (目前逻辑主要在 main.cpp，头文件被自动包含)
SOURCES = main.cpp

# 库文件链接
# -lpthread: 线程库
# -lmysqlclient: MySQL 客户端库
LIBS = -lpthread -lmysqlclient

# 默认执行的目标
all: $(TARGET)

# 链接生成可执行文件
$(TARGET): $(SOURCES)
	$(CXX) $(CXXFLAGS) $(SOURCES) -o $(TARGET) $(LIBS)

# 清理编译产生的文件
clean:
	rm -rf $(TARGET)
	rm -rf ./server.log

# 帮助信息
help:
	@echo "编译命令: make"
	@echo "清理命令: make clean"
	@echo "运行命令: ./server [port]"