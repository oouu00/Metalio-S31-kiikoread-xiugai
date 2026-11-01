# 构建与环境自包含（.idf 工具链）

## 为什么自包含
原编译依赖 `C:\Users\qa\Desktop\gujian\esp-idf-v6.1` 与 `C:\Users\qa\.espressif`（工具链 +
python venv）。这些被整包拷进项目 `.idf/`，`build_metalio.py` 改为相对 `.idf/` 路径，
换电脑只需拷走整个项目目录即可编译，不依赖 `C:\Users\qa`。

## .idf/ 结构（镜像 .espressif 顶层）
```
.idf/
├── esp-idf-v6.1/                 # IDF 本体（原 esp-idf-v6.1）
├── tools/                        # 工具链：riscv32-esp-elf, cmake, ninja, esp-rom-elfs, ...
├── python_env/
│   └── idf6.1_py3.13_env/        # python venv（Scripts/python.exe）
├── espidf.constraints.v6.1.txt   # IDF 约束文件（注意：无连字符！）
└── idf-env.json
```
注意 `IDF_TOOLS_PATH` 必须指向 `.idf` 顶层（含 `tools/`、`python_env/` 与约束文件），
**不是** `.idf/tools`。

## 编译命令
```
# 进入项目根
python build_metalio.py                           # 仅编译
python build_metalio.py -p                        # 编译 + 烧写 COM5
python build_metalio.py --ver 0.3.3-rc104n5 --sd # 编译 + 生成 TF 卡升级包
```
脚本内部用 `.idf/python_env/.../python.exe` 调
`idf.py -DKIIKO_BOARD=metalio47 --preview build`，自动设置
`IDF_PATH` / `IDF_TOOLS_PATH` / `IDF_PYTHON_ENV_PATH` / `ESP_ROM_ELF_DIR` / `PATH`。
`--ver` 会 touch 根 `CMakeLists.txt` 强制 cmake 重新配置。

## TF 卡升级包
- OTA 拒绝与当前版本相同的包（`ota_update.c`：`strcmp==0`）。**每次 `--ver` 必须不同**。
- 产物：项目根 `Metalio-S31-update.bin`（文件名不可改）。
- 设备端路径：`/sdcard/Metalio-S31-update.bin`；入口：设置 → 设备设置 → 固件升级。
- 校验：chip_id==32、项目名一致、版本不同；写入空闲 OTA 分区，失败回滚。
- 串口烧写偏移：bootloader 0x2000 / 分区表 0x8000 / otadata 0xd000 / app 0x10000，保留 NVS。

## 常见坑
- **约束文件名**：`espidf.constraints.v6.1.txt`（无连字符）。若缺失，cmake 报
  `.../esp-idf.constraints.v6.1.txt doesn't exist`。原增量编译靠缓存跳过该校验，全清后才暴露。
- **venv home**：venv 的 `pyvenv.cfg` `home` 指向系统 Python313。同机拷贝有效；
  换机若 Python 安装路径不同，编辑
  `.idf/python_env/idf6.1_py3.13_env/pyvenv.cfg` 的 `home` / `executable` / `base-prefix`
  指向新机 Python313。
- **移动目录 / 换工具链**：删 `build/` 重新完整编译（CMake 缓存含旧绝对路径）。
- **代理**：脚本检测 `127.0.0.1:26561` 代理，离线时组件下载会失败（正常编译不需要联网）。
- `build/` 与 `.idf/` 已在 `.gitignore` 排除，不进 git。
