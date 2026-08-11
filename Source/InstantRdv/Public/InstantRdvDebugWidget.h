/*
    InstantRdvDebugWidget.h
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
