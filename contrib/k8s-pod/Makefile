build:
	podman build -t passt -f Dockerfile .

run: build
	podman run --privileged --rm -it passt
