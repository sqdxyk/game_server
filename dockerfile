# 使用 Ubuntu 22.04 作为基础镜像
FROM ubuntu:22.04

# 安装编译工具和依赖库
RUN apt update && apt install -y \
    g++ \
    cmake \
    make \
    pkg-config \
    libboost-all-dev \
    libssl-dev \
    libmysqlclient-dev \
    && rm -rf /var/lib/apt/lists/*

# 设置工作目录
WORKDIR /app

# 复制项目源码到镜像中
COPY . .

# 编译
RUN rm -rf build && mkdir build && cd build && \
    cmake .. && \
    make -j$(nproc)

# 暴露服务器端口（根据你的实际端口修改）
EXPOSE 8080

# 运行服务器（可执行文件名为 main，根据 CMakeLists.txt）
CMD ["./build/main"]