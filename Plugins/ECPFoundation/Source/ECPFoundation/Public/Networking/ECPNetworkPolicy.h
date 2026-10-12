#pragma once

/** 同一采样/外推策略供角色和世界物理共用，避免两个功能仅因常量互相依赖。 */
namespace ECPNetworkPolicy
{
	inline constexpr float DefaultPoseInterval = 1.f / 30.f;
	inline constexpr float MaxExtrapolation = .04f;
}
