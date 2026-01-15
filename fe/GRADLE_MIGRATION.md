# FE Gradle 增量编译方案

## 状态

✅ **Gradle 迁移完成** - fe-core 和 fe-common 都可以使用 Gradle 编译。

## 使用方法

### 前提条件

1. **首次使用前，需要用 Maven 生成代码**：
   ```bash
   cd fe
   mvn generate-sources -pl fe-core -am -DskipTests -Dcheckstyle.skip=true
   ```
   这会生成 ANTLR、JFlex、CUP 和 Protobuf 代码。

### Gradle 编译

```bash
cd fe

# 编译 fe-core
./gradlew :fe-core:compileJava

# 编译所有模块
./gradlew compileJava

# 打包
./gradlew :fe-core:jar
```

### 编译时间

| 场景 | 时间 |
|------|------|
| 首次编译 (Gradle daemon 冷启动) | ~4 分钟 |
| 无更改重新编译 (UP-TO-DATE) | ~13 秒 |
| 修改单个文件后重新编译 | ~3-4 分钟 |

**注意**：由于 FE 代码库规模庞大（~4000+ Java 文件），增量编译仍需要一定时间来分析依赖关系。

## 快速开发工作流

对于日常开发，推荐以下工作流：

### 方案一：IntelliJ IDEA（推荐）

1. 打开 `fe/` 目录作为 Gradle 项目
2. 使用 IDEA 的增量编译：`Build → Build Project` (Ctrl+F9)
3. IDEA 的增量编译通常在 **1-3 秒** 内完成

### 方案二：增量编译脚本

使用 `incremental-compile.sh` 脚本（基于 Maven classpath + javac）：

```bash
cd fe

# 1. 首次初始化（只需运行一次）
./incremental-compile.sh --init

# 2. 增量编译（修改文件后）
./incremental-compile.sh              # 编译修改的文件
./incremental-compile.sh --watch      # 监控模式，自动编译

# 3. 编译单个文件
./incremental-compile.sh fe-core/src/main/java/org/apache/doris/YourFile.java
```

**预期效果：**
| 场景 | 编译时间 |
|------|----------|
| 单个文件 | < 1 秒 |
| 10 个文件 | 1-3 秒 |
| 100 个文件 | 5-10 秒 |

## 技术细节

### 项目结构

```
fe/
├── gradlew                    # Gradle wrapper
├── gradle/wrapper/
├── settings.gradle.kts        # 项目设置
├── build.gradle.kts           # 根配置 (版本管理)
├── gradle.properties          # Gradle 优化参数
├── fe-common/build.gradle.kts # fe-common 模块配置
├── fe-core/build.gradle.kts   # fe-core 模块配置
├── incremental-compile.sh     # 秒级增量编译脚本
├── fast-compile.sh            # Maven 快速编译脚本
└── GRADLE_MIGRATION.md        # 本文档
```

### 代码生成

Gradle 使用 Maven 预生成的代码，不自行生成：
- ANTLR4：`target/generated-sources/antlr4/`
- JFlex：`target/generated-sources/jflex/`
- CUP：`target/generated-sources/cup/`
- Protobuf：`target/generated-sources/org/`, `target/generated-sources/doris/`
- Thrift：`../gensrc/build/gen_java/`

### 已知问题和解决方案

#### 1. CUP Parser 兼容性

Maven 生成的 `SqlParser.java` 使用 `Stack<Symbol>`，但 `java-cup-runtime 0.11-a-czt01-cdh`
需要原始 `Stack`。Gradle 构建会自动打补丁修复此问题。

#### 2. 首次编译慢

首次编译需要下载依赖、启动 daemon，预计 4-5 分钟。后续编译会利用 daemon 和缓存。

#### 3. 完整构建仍需 Maven

如需运行测试、打包发布，建议使用 Maven：
```bash
mvn package -DskipTests -Dcheckstyle.skip=true
```

## 优化建议

### 1. 使用 Gradle Daemon

确保 `gradle.properties` 中启用了 daemon：
```properties
org.gradle.daemon=true
org.gradle.parallel=true
org.gradle.caching=true
```

### 2. 增加 JVM 内存

```properties
org.gradle.jvmargs=-Xmx4g -XX:+HeapDumpOnOutOfMemoryError -XX:+UseParallelGC
```

### 3. 使用 Build Scan

```bash
./gradlew :fe-core:compileJava --scan
```

这会生成详细的构建分析报告，帮助识别性能瓶颈。

## build.sh 集成

Gradle 增量编译已集成到 Doris 主构建脚本 `build.sh` 中。

### 使用方式

```bash
# 方式一：显式指定增量编译
./build.sh --fe-incremental

# 方式二：自动检测（需设置环境变量）
FE_INCREMENTAL_AUTO=1 ./build.sh --fe
```

### 工作原理

1. **--fe-incremental 选项**：强制使用 Gradle 增量编译
   - 自动检测并生成所需的源代码（ANTLR、CUP、JFlex、Protobuf）
   - 使用 Gradle 编译 Java 代码
   - 使用 Maven 打包 JAR（跳过编译步骤）
   - 如果 Gradle 失败，自动回退到 Maven 完整构建

2. **FE_INCREMENTAL_AUTO=1**：智能自动检测
   - 检查是否有之前的构建产物
   - 检查是否有语法文件（.g4, .flex, .cup, .proto, .thrift）变更
   - 如果条件满足，自动使用 Gradle 增量编译

### 推荐工作流

```bash
# 日常开发：使用增量编译
./build.sh --fe-incremental

# 完整构建：语法文件变更后
./build.sh --fe --clean

# CI/发布：使用标准 Maven 构建
./build.sh --fe
```

## 运行单元测试

### run-fe-ut.sh 优化选项

```bash
# 跳过代码生成（代码已生成时更快）
./run-fe-ut.sh --skip-gen --run org.apache.doris.YourTest

# 使用 Gradle 运行测试（增量更快）
./run-fe-ut.sh --gradle --run org.apache.doris.YourTest

# 指定并行线程数
./run-fe-ut.sh -j 4 --run org.apache.doris.YourTest

# 组合使用
./run-fe-ut.sh --skip-gen --gradle --run org.apache.doris.YourTest
```

### 直接使用 Gradle 运行测试

```bash
cd fe

# 运行指定测试类
./gradlew :fe-core:test --tests "org.apache.doris.YourTest"

# 运行指定测试方法
./gradlew :fe-core:test --tests "org.apache.doris.YourTest.testMethodName"

# 运行所有测试
./gradlew :fe-core:test
```

### 测试运行时间对比

| 方式 | 首次运行 | 增量运行（代码未变） |
|------|----------|----------------------|
| Maven (默认) | ~3-5 分钟 | ~2-3 分钟 |
| Maven --skip-gen | ~2-3 分钟 | ~1-2 分钟 |
| Gradle | ~2-3 分钟 | ~10-30 秒 |

**提示**：使用 `--skip-gen` 时确保代码已经生成过（运行过完整构建）。

## 快速参考

```bash
# ========== build.sh ==========
./build.sh --fe-incremental         # Gradle 增量编译
./build.sh --fe                     # Maven 完整编译
./build.sh --fe --clean             # 清理后重新编译

# ========== run-fe-ut.sh ==========
./run-fe-ut.sh --skip-gen --run TestClass   # 跳过代码生成
./run-fe-ut.sh --gradle --run TestClass     # 使用 Gradle
./run-fe-ut.sh -j 4 --run TestClass         # 4线程并行

# ========== Gradle ==========
./gradlew :fe-core:compileJava      # 编译
./gradlew :fe-core:test --tests "TestClass"  # 运行测试
./gradlew :fe-core:jar              # 打包
./gradlew clean                     # 清理

# ========== 秒级编译 ==========
./incremental-compile.sh --init     # 初始化（首次）
./incremental-compile.sh            # 增量编译
./incremental-compile.sh --watch    # 监控模式

# ========== Maven ==========
mvn compile -pl fe-core -am -DskipTests -Dcheckstyle.skip=true

# ========== IDEA ==========
# Ctrl+F9                           # 增量编译
```
