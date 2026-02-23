# cursor_mapper

Windows 多显示器跨屏鼠标百分比映射工具。

## 问题

Windows 扩展桌面模式下，当两块显示器分辨率或纵横比不同时，鼠标从一个屏幕边缘移到另一个屏幕会出现位置跳动。系统按绝对像素坐标裁剪，导致光标突然偏移。

## 解决方案

本工具在后台运行，通过低级鼠标钩子拦截跨屏移动，按边缘百分比重新映射光标位置。例如在左屏高度 80% 处越过边界，则在右屏高度 80% 位置进入。

## 构建

需要 CMake 3.20+、MSVC (Visual Studio 2022)、vcpkg。

```bash
# 设置 VCPKG_ROOT 或直接指定 toolchain
cmake --preset default
cmake --build build --config Release
```

## 运行

```bash
.\build\Release\cursor_mapper.exe
```

程序在控制台后台运行，Ctrl+C 退出。

## 技术要点

- **边缘检测** — 线段交点法：用上一帧→当前帧的移动向量与源屏矩形求交，角点 tie-break 按位移主轴决定
- **百分比映射** — 基于源屏与目标屏的共享边重叠区间计算百分比，映射到目标屏完整边，clamp + 向内收 1px 防抖动
- **递归防抖** — LLMHF_INJECTED 主保护 + g_suppressing 辅助保护，避免 SetCursorPos 触发的注入事件被重复映射
- **拓扑刷新** — WM_DISPLAYCHANGE + WM_SETTINGCHANGE + 30 秒定时器三路触发，基于拓扑签名（RECT + 主屏 + 设备名）去重
- **DPI 感知** — 嵌入 PerMonitorV2 manifest + 运行时 SetProcessDpiAwarenessContext 双重保障

## 系统要求

- Windows 10 1703+ (build 15063)
- 多显示器扩展桌面模式

## 项目结构

```
├── CMakeLists.txt       # 构建配置
├── CMakePresets.json    # vcpkg toolchain 集成
├── vcpkg.json           # vcpkg manifest
├── app.manifest         # DPI 声明
└── src/
    └── main.cpp         # 全部逻辑
```
