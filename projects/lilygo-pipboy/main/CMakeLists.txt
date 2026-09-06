set(srcs
	"main.c"
	"pipboy_ui.c"
)

idf_component_register(
	SRCS ${srcs}
	INCLUDE_DIRS "."
	# EMBED_FILES image.jpg
)
