# ECOCppLogger

C＋＋用の高速なログ生成

## ビルド

CMake 3.24以上とC++20のコンパイラでビルドできます（依存先は構成時に取得されます）。

```sh
cmake -S .ecobuild/.cppbuild -B build
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
```

---

[ECOBuild](https://github.com/superFUKA/ECOBuild) で管理しています。
