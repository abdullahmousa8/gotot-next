#include "register_types.h"

#include "core/os/memory.h"
#include "core/string/print_string.h"

#include "gotot_render.h"
#include "gotot_render_server.h"

static GototRenderServer *gotot_render_server = nullptr;

void initialize_gotot_render_module(ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_SERVERS) {
		return;
	}

	GototRender::initialize();

	GDREGISTER_CLASS(GototRenderServer);

	gotot_render_server = memnew(GototRenderServer);
	GototRenderServer::set_server_singleton(gotot_render_server);

	gotot_render_server->initialize();

	print_line("[GOTOT-NEXT] GototRenderServer initialized.");
}

void uninitialize_gotot_render_module(ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_SERVERS) {
		return;
	}

	if (gotot_render_server != nullptr) {
		gotot_render_server->shutdown();
		GototRenderServer::set_server_singleton(nullptr);
		memdelete(gotot_render_server);
		gotot_render_server = nullptr;
	}

	GototRender::shutdown();
}