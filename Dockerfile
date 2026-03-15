# ===== BUILDER (MinGW cross-compile) =====
FROM alpine:3.22 AS builder

RUN apk add --no-cache cmake make mingw-w64-gcc g++

WORKDIR /build
COPY CMakeLists.txt ./
COPY src/ src/

RUN mkdir build && cd build && \
    cmake .. \
        -DCMAKE_SYSTEM_NAME=Windows \
        -DCMAKE_C_COMPILER=x86_64-w64-mingw32-gcc \
        -DCMAKE_CXX_COMPILER=x86_64-w64-mingw32-g++ \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_CXX_FLAGS="-O2 -DNDEBUG -ffunction-sections -fdata-sections" \
        -DCMAKE_EXE_LINKER_FLAGS="-static -static-libgcc -static-libstdc++ -Wl,--gc-sections -Wl,-s" && \
    make -j$(nproc) && \
    x86_64-w64-mingw32-strip --strip-all tcbridge.exe

# ===== RUNTIME =====
FROM debian:bookworm-slim

RUN apt-get update && \
    apt-get install -y --no-install-recommends \
        wine64 \
        ca-certificates \
        tzdata && \
    rm -rf /var/lib/apt/lists/* \
           /usr/share/doc \
           /usr/share/man \
           /usr/share/info \
           /usr/share/lintian \
           /usr/share/locale \
           /usr/share/bug \
           /usr/share/common-licenses \
           /usr/share/wine/fonts \
           /var/log/* \
           /var/cache/* && \
    cd /usr/lib/x86_64-linux-gnu/wine/x86_64-windows && \
    rm -f \
        winemine.exe winefile.exe wordpad.exe winhlp32.exe write.exe \
        winemenubuilder.exe winebrowser.exe \
        winealsa.drv winepulse.drv wineoss.drv \
        wineusb.sys winehid.sys winexinput.sys winebus.sys \
        winevulkan.dll \
        d3d8.dll d3d9.dll d3d10.dll d3d10_1.dll d3d10core.dll \
        d3d11.dll d3d12.dll d3dcompiler_*.dll \
        dxgi.dll wined3d.dll opengl32.dll \
        wineps.drv \
        xinput1_1.dll xinput1_2.dll xinput1_3.dll xinput1_4.dll xinput9_1_0.dll \
    2>/dev/null; true

ENV WINEDEBUG=-all
ENV WINEPREFIX=/root/.wine
ENV XDG_RUNTIME_DIR=/tmp/runtime-root
ENV DISPLAY=
ENV TZ=Europe/Moscow
ENV PATH="/usr/lib/wine:${PATH}"

RUN mkdir -p /tmp/runtime-root && chmod 700 /tmp/runtime-root && \
    wine64 wineboot --init 2>/dev/null; \
    wineserver --wait 2>/dev/null || true && \
    rm -rf \
        /root/.wine/drive_c/windows/Installer \
        /root/.wine/drive_c/windows/Logs \
        /root/.wine/drive_c/windows/temp \
        /root/.wine/drive_c/windows/inf \
        /root/.wine/drive_c/windows/Help \
        /root/.wine/drive_c/windows/Microsoft.NET \
        /root/.wine/drive_c/users/root/Temp \
    2>/dev/null; \
    find /root/.wine -name "*.msi" -delete 2>/dev/null; \
    find /root/.wine -name "*.log" -delete 2>/dev/null; \
    find /root/.wine/drive_c/windows/Fonts -type f ! -name "tahoma*" -delete 2>/dev/null; \
    true

RUN mkdir -p /usr/bin/logs /usr/bin/dll_logs

COPY --from=builder /build/build/tcbridge.exe /usr/bin/tcbridge.exe
COPY txmlconnector64.dll /usr/bin/txmlconnector64.dll

WORKDIR /usr/bin

ENTRYPOINT ["wine64", "/usr/bin/tcbridge.exe"]