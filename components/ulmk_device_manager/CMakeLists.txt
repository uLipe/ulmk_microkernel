ulmk_component_register(
	NAME         ulmk_device_manager
	ENABLED      OFF
	SOURCES
		src/registry.c
		src/client.c
		src/serve.c
	INCLUDE_DIRS include
)
