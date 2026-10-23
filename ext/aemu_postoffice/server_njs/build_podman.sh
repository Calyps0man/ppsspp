set -xe

IMAGE=node:slim

podman run \
	--rm -it \
	-v ./:/work_dir \
	-w /work_dir \
	--entrypoint '/bin/bash' \
	$IMAGE \
	-c '
		npm install
		npm run build
	'
