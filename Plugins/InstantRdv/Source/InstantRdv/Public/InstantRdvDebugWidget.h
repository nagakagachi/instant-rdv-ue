/*
    InstantRdvDebugWidget.h

    CVarメタデータから生成されるInstant-RDVデバッグ操作パネルの
    Slate Widgetインターフェースを定義する。
*/

#pragma once

#include "Widgets/SCompoundWidget.h"

class INSTANTRDV_API SInstantRdvDebugPanel final : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SInstantRdvDebugPanel) {}
    SLATE_END_ARGS()

    void Construct(const FArguments& InArgs);

private:
    FReply ResetAllSettings();
};
