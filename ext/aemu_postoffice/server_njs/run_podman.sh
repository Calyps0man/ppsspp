IMAGE=node:slim

DEBUG=${DEBUG:-false}

podman_arg=""
node_arg=""
if $DEBUG
then
	podman_arg="-p 9229:9229"
	node_arg="--inspect=0.0.0.0:9229"
fi

MAX_OLD_SPACE_MB=${MAX_OLD_SPACE_MB:-}
if [ -n "$MAX_OLD_SPACE_MB" ]
then
	node_arg="$node_arg --max-old-space-size=$MAX_OLD_SPACE_MB"
fi

MAX_SEMI_SPACE_MB=${MAX_SEMI_SPACE_MB:-128}
if [ -n "$MAX_SEMI_SPACE_MB" ]
then
	node_arg="$node_arg --max-semi-space-size=$MAX_SEMI_SPACE_MB"
fi

podman run \
	--rm -it \
	-p 27313:27313 \
	-p 27314:27314 \
	-v ./aemu_postoffice.js:/aemu_postoffice.js:ro \
	-v ./config.json:/config.json:ro \
	-e AEMU_POSTOFFICE_CONFIG_PATH='/config.json' \
	$podman_arg \
	$IMAGE \
	${node_arg} /aemu_postoffice.js
