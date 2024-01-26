#!/usr/bin/env bash

if [ ! -d "./build/" ]; then
  echo "You must run ./change-compiler.sh first"
  exit 1
fi

cmake --build build --clean-first
