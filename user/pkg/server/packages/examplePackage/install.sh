#!/bin/sh

install_dir="$4"
mkdir -p "$install_dir"
cp "$1/payload/hello.txt" "$install_dir/hello.txt"
echo "[examplePackage] installed hello.txt to $install_dir"
