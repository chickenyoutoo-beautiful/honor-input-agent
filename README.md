# 荣耀输入代理（Honor Input Agent）

把荣耀平板智能键盘的**触控板**和**键盘**从系统手里接管过来，重新以标准输入设备上报，
解决串流/远程桌面场景下「触控板不能用、Win 键被吞、ESC 变返回」的问题。

---

## 一、适配机型

### 已验证

| 设备 | 型号 | 系统 | 状态 |
|---|---|---|---|
| 荣耀 MagicPad3 Pro 13.3 | `YLP-W00` | MagicOS / Android 17 | ✅ 完整验证 |
| 配套智能键盘 | HONOR Smart Touch Keyboard（pogo 触点，UHID 复合设备） | — | ✅ 完整验证 |

### 输入设备特征（判断是否适用）

适用设备在 `/proc/bus/input/devices` 中应能匹配到：

```
N: Name="HONOR Smart Touch Keyboard"    ← 键盘节点
N: Name="HONOR Smart Touch"             ← 触控板节点（INPUT_PROP_POINTER | INPUT_PROP_DIRECT）
```

如果触控板节点的 `input props` 里带 **`INPUT_PROP_DIRECT`**，就会出现本文要解决的问题。

### 未验证但可能适用

- 其他使用同款 HONOR Smart Touch Keyboard 的荣耀平板（MagicPad 2、MagicPad3 等）
- 判断方法：装上本 App，看「触控板代理」能否启动且指针变为相对移动

### 不适用

- 蓝牙/USB 普通鼠标（本来就是标准指针设备，无需代理）
- 传统笔记本形态的触控板（`INPUT_PROP_POINTER` 且无 `DIRECT`，系统已正确处理）

---

## 二、两种使用状态（重要）

荣耀这套键盘的输入行为**取决于手写笔是否连接**，这是固件层面的差异：

### 状态 A：手写笔已吸附/连接

系统会把键盘触控板识别为**标准指针设备**，此时：

- 触控板相对移动、双指滚动、指针捕获**原生正常**
- Win 键仍然被系统吞掉（PC 模式快捷键拦截）
- **不需要 root**，只需开启无障碍服务即可让 Win 键/ESC 直达主机

### 状态 B：手写笔未连接

HID 复合设备重新枚举后，触控板退回 **`INPUT_PROP_DIRECT`（触摸屏语义）**，此时：

- 指针变成绝对定位（点哪跳哪，抬手复位）
- `requestPointerCapture()` 失败 → 相对移动、双指滚动全部失效
- 键盘的 ESC 被上报为 `KEY_BACK`，Win 键被系统拦截
- **必须 root**，用本 App 的中继接管设备才能恢复

> 结论：**手写笔连着 = 开箱可用（仅需无障碍）；手写笔不连 = 需要 root + 本 App。**

---

## 三、工作原理

App 内置两个 ARM64 原生中继，通过 `su` 启动：

```
物理触控板 /dev/input/event12
        │  EVIOCGRAB 独占
        ▼
  touchpad_relay
        │  重新上报为虚拟鼠标
        │  REL_X / REL_Y / REL_WHEEL_HI_RES / BTN_LEFT / BTN_RIGHT
        ▼
   honor-touchpad-relay（无 INPUT_PROP_DIRECT）

物理键盘 /dev/input/event10
        │  EVIOCGRAB 独占
        ▼
    kbd_relay
        │  KEY_BACK  → KEY_ESC
        │  KEY_META  → KEY_RED（避开荣耀 PC 模式快捷键）
        ▼
   honor-keyboard-relay
        │
        ▼
   Moonlight 将 KEY_RED 翻译为 VK_LWIN，发给主机
```

**关键点**：`KEY_RED`（`PROG_RED`）是系统几乎不用的键码，所以荣耀的
`Win+Shift+S` 等快捷键不会再触发，但按键依然能正常传到主机。

---

## 四、功能

| 功能 | 说明 |
|---|---|
| 相对指针 | 单指滑动移动指针（灵敏度可调 0.5×–4.0×） |
| 双指滚动 | 高精度滚轮（`REL_WHEEL_HI_RES`），支持方向反转 |
| 单指轻点 | 左键 |
| 双指轻点 | 右键 |
| 物理按压 | 原样透传（可拖动、框选） |
| ESC 直通 | 内核层改为 `KEY_ESC`，不再触发返回 |
| Win 键直通 | 改为 `KEY_RED` 绕过拦截，App 侧翻译为 `VK_LWIN` |

触控板与键盘**各自独立开关**，可单独启停。

---

## 五、使用

1. 安装 APK，打开 App
2. 首次启动会请求 **root**，允许
3. 打开「触控板代理」和/或「键盘代理」开关
4. 拖动灵敏度/滚动速度滑杆即时生效（会自动重启对应中继）
5. 关闭开关 → 中继进程退出 → 设备释放 → **系统立刻恢复原生行为**

### 权限说明

| 权限 | 用途 |
|---|---|
| root (su) | 访问 `/dev/input/*`、`/dev/uinput`，独占并重建输入设备 |
| 前台服务 | 保持中继常驻（KernelSU 的 su 会话结束时子进程会被清理） |
| 通知 | 前台服务的常驻通知 |

---

## 六、已知限制

- **只支持 ARM64**（`arm64-v8a`）
- **必须 root**：普通应用无法访问 `/dev/uinput`，也无权独占输入设备（Android 安全边界）
- **无障碍服务无法替代**：无障碍只能过滤按键、模拟触摸手势，无法产生滚轮事件或接管设备
- 中继运行期间，该输入设备对系统不可见（被 `EVIOCGRAB` 独占）；App 被杀会一并释放
- 刷新率限制（手写笔连接时锁定 120Hz）**不在本 App 处理范围**

---

## 七、构建

```bash
# 需要 Android SDK + NDK r27 + JDK 21
cd HonorInputAgent
./gradlew assembleDebug
```

原生中继源码在 `native/` 目录：

```bash
aarch64-linux-android21-clang -O2 -o touchpad_relay native/touchpad_relay.c
aarch64-linux-android21-clang -O2 -o kbd_relay      native/kbd_relay.c
```

把产物复制到 `app/src/main/assets/` 即可参与打包。

---

## 八、相关

- Moonlight 侧的适配补丁见 PR：<https://github.com/Axixi2233/moonlight-android/pull/87>
- 补丁内容：键盘越界崩溃修复、ESC 重映射、指针捕获恢复、本地光标、`ACTION_SCROLL` 处理、帧率声明

---

## 九、配套 Xposed 模块（实验性）

仓库同源的 `HonorHook` 模块用于在框架层处理输入/显示策略，**尚未验证稳定**。

### 作用域要求

在 LSPosed / Vector 中启用该模块时，**作用域必须勾选「Android 系统」（Android System）**：

```
模块 → HonorHook → 作用域
  ☑ Android 系统（android）
  ☐ 其他应用（可选）
```

原因：需要拦截的处理都在 `system_server` 进程内完成——

| 目标 | 所在进程 |
|---|---|
| 荣耀 PC 模式快捷键拦截（Win 键被吞） | `system_server` |
| 显示模式/刷新率决策（120Hz 锁） | `system_server` |
| 键盘分发的 BACK 重映射 | `system_server` |

只作用于普通应用进程是**无效**的。

### 风险提示

- 模块代码在 `system_server` 内运行，**任何异常都会导致系统服务崩溃重启**
- 已验证：模块带异常时会导致 system_server 反复崩溃，进而 Vector 守护进程与 Wi-Fi 服务都起不来
- 建议：先确认框架（LSPosed / Vector）在本机可正常启动，再启用本模块；出问题通过 KernelSU 关闭模块或删除 `/data/adb/modules/<框架>/` 下的模块后重启

### 框架兼容性

| 框架 | 本机（MagicPad3 Pro 13.3 / Android 17）实测 |
|---|---|
| LSPosed 2.2.1 | ❌ `lspd` 启动即 SIGSEGV（`fault addr 0x314`） |
| Vector 2.2 (JingMatrix) | ⚠️ 不带 Hook 模块时可正常启动；带模块的系统作用域注入会导致系统服务异常 |