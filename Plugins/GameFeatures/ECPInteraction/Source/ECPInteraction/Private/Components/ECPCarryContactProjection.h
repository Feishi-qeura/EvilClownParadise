#pragma once
#include "CoreMinimal.h"

namespace ECPCarryContacts
{
/** Normal 为单位向量；MinimumDot <= 0，当前抓取点始终是可行位置。 */
struct FPlaneConstraint
{
	FVector Normal;
	double MinimumDot=0.0;
};
inline FVector ProjectDelta(FVector Desired,TConstArrayView<FPlaneConstraint> Planes)
{
	auto Feasible=[&](const FVector& Point)
	{
		for (const FPlaneConstraint& Plane : Planes)
			if (FVector::DotProduct(Point,Plane.Normal)<Plane.MinimumDot-0.0001) return false;
		return true;
	};
	if (Feasible(Desired)) return Desired;
	FVector Best=FVector::ZeroVector;
	double BestDistance=Desired.SizeSquared();
	auto Consider=[&](const FVector& Point)
	{
		if (Point.ContainsNaN() || !Feasible(Point)) return;
		const double Distance=(Point-Desired).SizeSquared();
		if (Distance<BestDistance) { Best=Point; BestDistance=Distance; }
	};
	// 三维凸半空间的最近点最多需要三个独立活跃面。枚举面/棱/顶点，
	// 一次满足所有已遇到的墙和角色约束，不依赖投影顺序，也不丢失可行的上抬分量。
	for (int32 A=0;A<Planes.Num();++A)
	{
		const FPlaneConstraint& First=Planes[A];
		const double D1=First.MinimumDot-FVector::DotProduct(Desired,First.Normal);
		Consider(Desired+First.Normal*D1);
		for (int32 B=A+1;B<Planes.Num();++B)
		{
			const FPlaneConstraint& Second=Planes[B];
			const double Dot=FVector::DotProduct(First.Normal,Second.Normal);
			const double Determinant=1.0-Dot*Dot;
			if (Determinant>1.e-9)
			{
				const double D2=Second.MinimumDot-FVector::DotProduct(Desired,Second.Normal);
				Consider(Desired+First.Normal*((D1-Dot*D2)/Determinant)+Second.Normal*((D2-Dot*D1)/Determinant));
			}
			for (int32 C=B+1;C<Planes.Num();++C)
			{
				const FPlaneConstraint& Third=Planes[C];
				const FVector Cross23=FVector::CrossProduct(Second.Normal,Third.Normal);
				const double Volume=FVector::DotProduct(First.Normal,Cross23);
				if (FMath::Abs(Volume)<1.e-9) continue;
				Consider((Cross23*First.MinimumDot+FVector::CrossProduct(Third.Normal,First.Normal)*Second.MinimumDot
					+FVector::CrossProduct(First.Normal,Second.Normal)*Third.MinimumDot)/Volume);
			}
		}
	}
	return Best;
}
}
