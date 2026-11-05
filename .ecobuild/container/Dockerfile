# このモジュールの作業空間のコンテナの環境（ECOBuild がこれを元にイメージを作る）。
# モジュールで要るツールをここに足す。ECOBuild 自身（エージェント・ecobuild 等）は ECOBuild が上に重ねるので書かない。
# apt の使える Ubuntu・Debian を元にする（ECOBuild の層が apt と uid 1000 の利用者を使う）。
FROM ubuntu:24.04
RUN apt-get update \
 && DEBIAN_FRONTEND=noninteractive apt-get install --yes --no-install-recommends cmake ninja-build g++ \
 && rm -rf /var/lib/apt/lists/*
