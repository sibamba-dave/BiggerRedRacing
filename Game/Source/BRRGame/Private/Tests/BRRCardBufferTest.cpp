#include "BRRCardBuffer.h"

#include "Misc/AutomationTest.h"
#include "ProceduralMeshComponent.h"

#if WITH_DEV_AUTOMATION_TESTS

// The card buffer counts in triangles: the original's effects are triangle fans, not quads, and a
// frame with an odd triangle count must not lose its last triangle (it did when the buffer
// grouped blindly by six vertices).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBRRCardBufferTest, "BRR.Game.CardBuffer",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBRRCardBufferTest::RunTest(const FString& Parameters)
{
	UProceduralMeshComponent* Mesh = NewObject<UProceduralMeshComponent>();
	FBRRCardBuffer Cards;
	Cards.Create(Mesh, 0, 8);
	FProcMeshSection* Section = Mesh->GetProcMeshSection(0);
	if (!TestNotNull(TEXT("section exists"), Section)) return false;
	TestEqual(TEXT("capacity in vertices"), Section->ProcVertexBuffer.Num(), 8 * 3);

	// Three triangles - an odd count no quad grouping can hold - all three must land.
	const FVector2D Uv[3] = { {0, 0}, {1, 0}, {0, 1} };
	const FColor C[3] = { FColor::Red, FColor::Red, FColor::Red };
	Cards.Begin();
	for (int32 T = 0; T < 3; ++T)
	{
		const FVector P[3] = { FVector(T, 0, 0), FVector(T, 100, 0), FVector(T, 0, 100) };
		TestTrue(TEXT("triangle accepted"), Cards.AddTri(P, Uv, C, FVector::UpVector));
	}
	Cards.Commit(Mesh, 0);
	Section = Mesh->GetProcMeshSection(0);
	TestEqual(TEXT("three triangles live"), int32(Section->ProcVertexBuffer[6].Position.X), 2);
	TestTrue(TEXT("third triangle not collapsed"), Section->ProcVertexBuffer[7].Position != Section->ProcVertexBuffer[6].Position);
	TestTrue(TEXT("tail collapsed"), Section->ProcVertexBuffer[9].Position == Section->ProcVertexBuffer[10].Position);

	// A quad takes two triangles; past the capacity, adds are refused, never mangled.
	Cards.Begin();
	for (int32 Q = 0; Q < 4; ++Q)
		Cards.AddQuad(FVector(Q, 0, 0), FVector(Q, 1, 0), FVector(Q, 0, 1), FVector(Q, 1, 1),
			Uv[0], Uv[1], Uv[2], FVector2D(1, 1), FColor::White, FColor::White, FVector::UpVector);
	const FVector P[3] = { FVector::ZeroVector, FVector(1, 0, 0), FVector(0, 1, 0) };
	TestFalse(TEXT("full buffer refuses"), Cards.AddTri(P, Uv, C, FVector::UpVector));
	TestEqual(TEXT("used counts triangles"), Cards.Used, 8);
	return true;
}

#endif
