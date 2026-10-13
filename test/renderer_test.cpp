#include <gtest/gtest.h>

#include "config.h"
#include "engine/surface.hpp"

namespace devilution {

TEST(Renderer, FactorySelection)
{
	EXPECT_NE(CreateRenderer(RendererBackend::Legacy), nullptr);
	EXPECT_EQ(CreateRenderer(static_cast<RendererBackend>(-1)), nullptr);
	auto renderer = CreateRenderer(RendererBackend::WebGPU);
#if DEVILUTIONX_WEBGPU
	ASSERT_NE(renderer, nullptr);
	RendererConfig config;
	config.backend = RendererBackend::WebGPU;
	OutputSurface surface;
	EXPECT_FALSE(renderer->Initialize(config, &surface));
	EXPECT_FALSE(renderer->BeginFrame());
	EXPECT_FALSE(renderer->Present());
#else
	EXPECT_EQ(renderer, nullptr);
#endif
}

TEST(Renderer, HeadlessLifecycle)
{
	auto renderer = CreateRenderer(RendererBackend::Headless);
	ASSERT_NE(renderer, nullptr);
	RendererConfig config;
	EXPECT_FALSE(renderer->Initialize(config, nullptr));
	config.backend = RendererBackend::Headless;
	EXPECT_FALSE(renderer->BeginFrame());
	EXPECT_FALSE(renderer->Present());
	ASSERT_TRUE(renderer->Initialize(config, nullptr));
	EXPECT_FALSE(renderer->Initialize(config, nullptr));
	EXPECT_TRUE(renderer->BeginFrame());
	EXPECT_FALSE(renderer->BeginFrame());
	EXPECT_EQ(renderer->FrameSurface(), nullptr);
	EXPECT_TRUE(renderer->Present());
	renderer->EndFrame();
	EXPECT_FALSE(renderer->Present());
	renderer->Shutdown();
	renderer->Shutdown();
	EXPECT_FALSE(renderer->BeginFrame());
	EXPECT_TRUE(renderer->Initialize(config, nullptr));
	renderer->Shutdown();
}

} // namespace devilution
