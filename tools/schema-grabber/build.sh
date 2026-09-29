#/bin/bash

SCRIPT_DIR="$(dirname "$(realpath "$0")")"
cd "$SCRIPT_DIR"

dotnet publish -r linux-x64
