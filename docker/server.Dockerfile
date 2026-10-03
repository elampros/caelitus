# The caelitus C++ server.
#   docker build -f docker/server.Dockerfile -t caelitus-server .
#
# Stage 1 builds MariaDB Connector/C++ (not packaged by Ubuntu) and the
# server; stage 2 is a slim runtime with just the shared libraries.

FROM ubuntu:24.04 AS build
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake git ca-certificates \
        libmariadb-dev libspdlog-dev nlohmann-json3-dev libasio-dev libmosquitto-dev \
    && rm -rf /var/lib/apt/lists/*

ARG CONNCPP_VERSION=1.1.8
RUN git clone --depth 1 --branch "${CONNCPP_VERSION}" \
        https://github.com/mariadb-corporation/mariadb-connector-cpp.git /src/conncpp \
    && cmake -S /src/conncpp -B /src/conncpp/build -DCMAKE_BUILD_TYPE=Release \
        -DUSE_SYSTEM_INSTALLED_LIB=ON -DWITH_UNIT_TESTS=OFF -DCMAKE_INSTALL_PREFIX=/usr/local \
    && cmake --build /src/conncpp/build -j3 \
    && cmake --install /src/conncpp/build

COPY CMakeLists.txt CMakePresets.json /src/caelitus/
COPY cmake /src/caelitus/cmake
COPY include /src/caelitus/include
COPY src /src/caelitus/src
COPY docs/CMakeLists.txt /src/caelitus/docs/CMakeLists.txt
RUN cmake -S /src/caelitus -B /src/caelitus/build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF \
    && cmake --build /src/caelitus/build -j3 --target caelitus \
    && strip /src/caelitus/build/src/caelitus \
    && mkdir -p /out/lib \
    && cp -P $(find /usr/local/lib* -name 'libmariadbcpp.so*') /out/lib/

FROM ubuntu:24.04
RUN apt-get update && apt-get install -y --no-install-recommends \
        libmariadb3 libspdlog1.12 libmosquitto1 tzdata ca-certificates \
    && rm -rf /var/lib/apt/lists/*
COPY --from=build /out/lib/ /usr/local/lib/
RUN ldconfig
COPY --from=build /src/caelitus/build/src/caelitus /usr/local/bin/caelitus
COPY docker/config.json /etc/caelitus/config.json
USER nobody
EXPOSE 9000
ENTRYPOINT ["caelitus", "--config", "/etc/caelitus/config.json"]
