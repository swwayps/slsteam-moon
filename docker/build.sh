#!/bin/bash

DOCKER_HOST="sls-host"

SCRIPT_DIR="$(dirname "$(realpath "$0")")"
cd "$SCRIPT_DIR"

docker build -t $DOCKER_HOST .
docker run \
	--name $DOCKER_HOST \
	--user=sls \
	--rm \
	--mount=type=bind,source=../,target=/src \
	--workdir=/src \
	sls-host:latest \
	make $@
	#env MAKEFLAGS="-j$(nproc)" make $@
