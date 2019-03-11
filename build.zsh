#!/bin/zsh
#
# How to build:
#
#   mkdir /tmp/gitstatus && cd /tmp/gitstatus && zsh -c "$(curl -fsSL https://raw.githubusercontent.com/romkatv/gitstatus/master/build.zsh)"

readonly GITSTATUS_REPO_URL=https://github.com/romkatv/gitstatus.git
readonly LIBGIT2_REPO_URL=https://github.com/romkatv/libgit2.git

emulate -L zsh
setopt err_return err_exit no_unset pipe_fail

[[ $# == 1 && -n $1 ]] || {
  echo "Usage: build.sh DIR" >&2
  return 1
}

local DIR=${1:a}
local OS && OS=$(uname -s)
local CPUS && CPUS=$(getconf _NPROCESSORS_ONLN)

function prepare() {
  [[ ! -d $DIR ]] || {
    echo "directory exists: $DIR" >&2
    echo "remove it with \`rm -rf $DIR\` and retry" >&2
    return 1
  }
  mkdir $DIR
}

function build_libgit2() {
  cd $DIR
  git clone $LIBGIT2_REPO_URL
  mkdir libgit2/build
  cd libgit2/build
  cmake                        \
    -DCMAKE_BUILD_TYPE=Release \
    -DTHREADSAFE=ON            \
    -DUSE_BUNDLED_ZLIB=ON      \
    -DUSE_ICONV=OFF            \
    -DBUILD_CLAR=OFF           \
    -DUSE_SSH=OFF              \
    -DUSE_HTTPS=OFF            \
    -DBUILD_SHARED_LIBS=OFF    \
    -DUSE_EXT_HTTP_PARSER=OFF  \
    ..
  make -j $CPUS
}

function build_gitstatus() {
  cd $DIR
  git clone $GITSTATUS_REPO_URL
  cd gitstatus
  local cxxflags=${CXXFLAGS:-''}
  local ldflags=${LDFLAGS:-''};
  cxxflags+=" -I$DIR/libgit2/include"
  ldflags+=" -L$DIR/libgit2/build"
  case $OS in
    Linux)
      ldflags+=" -static-libstdc++ -static-libgcc"
      ;;
    FreeBSD)
      ldflags+=" -static"
  esac
  CXXFLAGS=$cxxflags LDFLAGS=$ldflags make -j $CPUS
  strip -s gitstatusd
  local arch && arch=$(uname -m)
  local target=bin/gitstatusd-${OS:l}-${arch:l}
  cp -f gitstatusd $target
  echo "built: $target" >&2
}

function verify_gitstatus() {
  local reply
  echo -nE $'hello\x1f\x1e' | $DIR/gitstatus/gitstatusd 2>/dev/null | {
    IFS='' read -r -d $'\x1e' -t 5 reply
    [[ $reply == $'hello\x1f0' ]]
  }
  echo "self-check successful" >&2
}

prepare
build_libgit2
build_gitstatus
verify_gitstatus