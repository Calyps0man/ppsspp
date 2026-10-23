IMAGE_NAME="pspsdk_aemu_postoffice"

if [ "$REBUILD_IMAGE" == "true" ] && podman image exists $IMAGE_NAME
then
	podman image rm -f $IMAGE_NAME
fi

if ! podman image exists $IMAGE_NAME
then
	podman image build -f Dockerfile -t $IMAGE_NAME
fi

podman run \
	--rm -it \
	--security-opt label=disable \
	-v ../:/workdir \
	-w /workdir/client \
	$IMAGE_NAME \
	-c '
	set -xe
	bash build_linux.sh
	bash build_psp.sh
	bash build_windows.sh
'
