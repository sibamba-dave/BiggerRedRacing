#pragma once

#include "CoreMinimal.h"
#include "BRRScn.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/HUD.h"
#include "BRRFrontEnd.generated.h"

class ABRRFrontEndGameMode;
class UBRRGameInstance;
class UTexture2D;

// The original's menu alphabets: SML_ALPH (8x8) and the five drawings of the wibbly WIBS, each
// glyph a texture in ALWAYS.WAP's DEFAULT.COL (white, so a ramp colour tints it).
struct FBRRMenuFont
{
	TArray<TObjectPtr<UTexture2D>> Glyphs;
	TArray<FIntPoint> Sizes;
	int32 Height = 8;
};

// One menu screen: an .SCN laid out on the 320x200 screen, and what its boxes do. The framework
// draws the picture (a backdrop in the 3D view, so models can stand in front of it), the
// anisprites and stexts of the visible groups, a highlight on the box under the pointer or the
// keyboard focus, and the pointer; a screen adds its own drawing and decides what a box does.
class FBRRMenuScreen : public TSharedFromThis<FBRRMenuScreen>
{
public:
	virtual ~FBRRMenuScreen() = default;
	ABRRFrontEndGameMode* Menu = nullptr;
	FBRRScn Scn;
	TSet<int32> HiddenGroups;      // stext/anisprite groups not shown
	TSet<FString> HiddenKeys;      // single stexts not shown (a screen draws its own words there)
	TArray<int32> FocusOrder;      // the box ids the arrow keys walk, in order
	int32 Focus = 0;               // index into FocusOrder
	TArray<UTexture2D*> SpriteTextures; // Scn.Sprites as textures (owned by the front end)

	virtual bool Load() = 0;       // fills Scn (and the rest); false: the screen cannot show
	virtual void Enter() {}        // on top again
	virtual void Tick(float DeltaSeconds) {}
	virtual void Draw(class FBRRMenuPainter& P) {}
	virtual void OnBox(int32 Id) = 0;
	virtual void OnBack() {}       // Escape, or the hidden corner box
	virtual void OnLeftRight(int32 Dir) {}
	virtual void Leave() {}        // popped or covered
};

// Draws on the 320x200 screen the way the original did, wherever the 4:3 picture sits.
class FBRRMenuPainter
{
public:
	static constexpr int32 FocusRamp = 5;
	UCanvas* Canvas = nullptr;
	ABRRFrontEndGameMode* Menu = nullptr;
	FVector2D Origin = FVector2D::ZeroVector;
	FVector2D Scale = FVector2D(1, 1); // screen pixels per original pixel

	FVector2D ToScreen(double X, double Y) const { return Origin + FVector2D(X * Scale.X, Y * Scale.Y); }
	void Image(UTexture2D* Texture, double X, double Y, double W = -1, double H = -1, FLinearColor Tint = FLinearColor::White);
	// A run of text at (X, Y) in font Font (0/1 SML_ALPH, 2.. WIBS), in palette ramp Ramp (-1 white).
	double Text(const FString& Words, double X, double Y, int32 Font, int32 Ramp, const TArray<FColor>& Palette, int32 WibblePhase = 0);
	// An entry of TOURN.<LANG>, laid out by its own `@X`/`@C`; At offsets it, Ramp overrides.
	void Entry(const FString& Key, int32 Font, const TArray<FColor>& Palette, FVector2D At = FVector2D::ZeroVector, int32 Ramp = -2, int32 WibblePhase = 0);
	void Box(double X, double Y, double W, double H, FLinearColor Colour);
	static double TextWidth(ABRRFrontEndGameMode* Menu, const FString& Words, int32 Font);
};

UCLASS()
class BRRGAME_API ABRRMenuHUD : public AHUD
{
	GENERATED_BODY()
public:
	virtual void DrawHUD() override;
};

// The front end: the original's menus, from the title to the start line and back. Its own world
// is empty but for a camera, the screen's picture on a backdrop and whatever model a screen shows.
UCLASS()
class BRRGAME_API ABRRFrontEndGameMode : public AGameModeBase
{
	GENERATED_BODY()

public:
	ABRRFrontEndGameMode();
	virtual void StartPlay() override;
	virtual void Tick(float DeltaSeconds) override;
	virtual void EndPlay(const EEndPlayReason::Type Reason) override;

	UBRRGameInstance* Game() const;
	FString DataDir() const;

	void Push(TSharedRef<FBRRMenuScreen> Screen);
	void Pop();
	void Replace(TSharedRef<FBRRMenuScreen> Screen);
	void PopToRoot();
	TSharedPtr<FBRRMenuScreen> Top() const { return Stack.IsEmpty() ? TSharedPtr<FBRRMenuScreen>() : TSharedPtr<FBRRMenuScreen>(Stack.Last()); }
	void Click();             // the original's select sound

	// The picture behind everything (the 3D backdrop), and textures made from the original's images.
	void ShowPicture(const FBRRImage& Picture);
	// A video's frame on the backdrop, through one texture updated in place.
	void ShowFrame(const FBRRImage& Frame);
	// bArt: interface art (fonts, sprites), smoothed in the remastered look (BRRArt).
	UTexture2D* MakeTexture(const FBRRImage& Image, bool bArt = false);
	bool bArtSmooth = false;
	const FBRRMenuFont& Font(int32 Index) const; // 0/1 SML_ALPH, 2..6 WIBS1..5
	double Seconds() const { return Clock; }

	// A model on the stage in front of the picture (the car screen's vehicle).
	UPROPERTY()
	TObjectPtr<AActor> Stage;
	FVector StagePoint() const;

	void Draw(UCanvas* Canvas);

private:
	TArray<TSharedRef<FBRRMenuScreen>> Stack;
	TArray<FBRRMenuFont> Fonts;
	TArray<FColor> DefaultPalette;
	double Clock = 0;
	FVector2D Pointer = FVector2D(-1, -1);   // in 320x200
	FVector2D LastMouse = FVector2D(-1, -1);
	bool bMouseMoved = false;
	FVector2D Origin, Scale;

	UPROPERTY()
	TObjectPtr<class ACameraActor> Camera;
	UPROPERTY()
	TObjectPtr<class AStaticMeshActor> Backdrop;
	UPROPERTY()
	TObjectPtr<class UMaterialInstanceDynamic> BackdropMaterial;
	UPROPERTY()
	TObjectPtr<class ABRRLook> Look;
	UPROPERTY()
	TArray<TObjectPtr<UTexture2D>> Textures;
	UPROPERTY()
	TObjectPtr<class UBRRSoundOutput> SoundOutput;
	UPROPERTY()
	TObjectPtr<UTexture2D> PointerTexture;
	UPROPERTY()
	TObjectPtr<UTexture2D> FrameTexture;

	void LoadFonts();
	// -BRRMenuWalk="0,3,..." presses those boxes a second apart and shoots each screen
	// (Menu_<n>.png), then quits: a check of the screens with no hand on the keyboard.
	TArray<int32> Walk;
	int32 WalkStep = -1;
	double WalkAt = 0;
	void UpdateWalk();
	int32 BoxAt(const FBRRMenuScreen& S, FVector2D At) const;
	void HandleInput();
};
