#include "register_types.h"

#include "core/os/memory.h"
#include "core/string/print_string.h"

#include "gne_render.h"
#include "gne_render_server.h"

static GneRenderServer *gne_render_server = nullptr;

void initialize_gne_render_module(ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_SERVERS) {
		return;
	}

	GneRender::initialize();

	GDREGISTER_CLASS(GneRenderServer);

	gne_render_server = memnew(GneRenderServer);
	GneRenderServer::set_server_singleton(gne_render_server);

	gne_render_server->initialize();

	print_line("[GNE] GneRenderServer initialized.");
}

void uninitialize_gne_render_module(ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_SERVERS) {
		return;
	}

	if (gne_render_server != nullptr) {
		gne_render_server->shutdown();
		GneRenderServer::set_server_singleton(nullptr);
		memdelete(gne_render_server);
		gne_render_server = nullptr;
	}

	GneRender::shutdown();
}