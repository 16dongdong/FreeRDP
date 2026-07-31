include(WarnUnmaintained)
warn_unmaintained("windows shadow server subsystem" "-DWITH_SHADOW_SUBSYSTEM=OFF")

add_compile_definitions(WITH_SHADOW_WIN)
add_library(
  freerdp-shadow-subsystem-impl STATIC
  win_dxgi.c
  win_dxgi.h
  win_audio.c
  win_audio.h
  win_brightness.c
  win_brightness.h
  win_shadow.c
  win_shadow.h
)
# DXGI 桌面复制避免 WDS 的单会话和端口占用限制，是物理桌面共享的首选后端。
target_compile_definitions(freerdp-shadow-subsystem-impl PRIVATE WITH_DXGI_1_2)
target_link_libraries(freerdp-shadow-subsystem-impl PRIVATE freerdp-client freerdp winpr ole32
                                                               oleaut32 wbemuuid mmdevapi uuid ksuser)
