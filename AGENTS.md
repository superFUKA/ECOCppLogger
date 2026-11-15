# AGENTS.md

このリポジトリ（ECOCppLogger、型：cpp）は ECOBuild で管理しています（このファイルは ECOBuild が作り直します。モジュールだけの決まりは CLAUDE.md 等に書きます）。

- 作業を始める前に `ecobuild agent guide` を実行し、手順を読んでください（このPCの ECOBuild に合った手順が出ます）。
- gitを直接使わず `ecobuild` コマンドを使います（コミットは `ecobuild task commit`、テストは `ecobuild test`）。
- ファイルを変えるのは、タスクの作業空間（`ecobuild task start <番号>` が作る `.ecobuild/w/<番号>`）の中でだけです。元の clone（`.ecobuild/w/` の外）
  では作業しません。元の clone で始めたら、`ecobuild agent guide` が示す今ある作業空間を見て、続きの作業ならその中へ移ります。
- 作業空間を消す操作（`ecobuild task finish`・`task drop`・`task prune` 等）は、シェルを元の clone へ移してから行います（中にいると消せずに残ります）。
- ECOBuild が起動したエージェント（MCP のツール `ecobuild` を渡されたもの）は、この手順の `ecobuild` コマンドの代わりに
  そのツールを使います。
- コミットのメッセージ・PR・Issue・コメントに、AIの署名（`Co-Authored-By: <AI>`・`Generated with …` 等）を書きません。
- 文書（AGENTS.md・README・コメント 等）と今のコードで書き方が食い違うときはコードに従い、見つけた食い違いは最後の報告（提出の説明 `--note`）にすべて書きます。
- CppBuildの生成ファイル（.ecobuild/.cppbuild/ の中の CMakeLists.txt・CppBuildTopLevel.cmake）は手で編集しません。
- 失敗したら、結果の `error.code` と `error.hint` に従ってください。
