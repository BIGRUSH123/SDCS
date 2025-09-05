FROM ubuntu:20.04

# 设置非交互模式
ENV DEBIAN_FRONTEND=noninteractive

# 安装必要的包
RUN apt-get update && apt-get install -y \
    g++ \
    make \
    && rm -rf /var/lib/apt/lists/*

# 创建工作目录
WORKDIR /app

# 复制源代码
COPY main.cpp .
COPY httplib.h .
COPY json.hpp .
COPY Makefile .

# 编译程序
RUN make

# 暴露端口
EXPOSE 9527 9528 9529

# 启动命令（将通过docker-compose覆盖）
ENTRYPOINT ["./cache_server"]
