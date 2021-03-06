// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ash/wm/overview/scoped_overview_animation_settings_aura.h"

#include "base/time/time.h"
#include "ui/aura/window.h"
#include "ui/compositor/layer.h"
#include "ui/compositor/layer_animation_observer.h"
#include "ui/compositor/scoped_layer_animation_settings.h"
#include "ui/gfx/animation/tween.h"

namespace ash {

namespace {

// The time duration for transformation animations.
const int kTransitionMilliseconds = 300;

// The time duration for fading out when closing an item.
const int kCloseFadeOutMilliseconds = 50;

// The time duration for scaling down when an item is closed.
const int kCloseScaleMilliseconds = 100;

// The time duration for widgets to fade in.
const int kFadeInMilliseconds = 60;

// The time duration for widgets to fade out.
const int kFadeOutDelayMilliseconds = kTransitionMilliseconds * 1 / 5;
const int kFadeOutMilliseconds = kTransitionMilliseconds * 3 / 5;

base::TimeDelta GetAnimationDuration(OverviewAnimationType animation_type) {
  switch (animation_type) {
    case OVERVIEW_ANIMATION_NONE:
      return base::TimeDelta();
    case OVERVIEW_ANIMATION_ENTER_OVERVIEW_MODE_FADE_IN:
      return base::TimeDelta::FromMilliseconds(kFadeInMilliseconds);
    case OVERVIEW_ANIMATION_EXIT_OVERVIEW_MODE_FADE_OUT:
      return base::TimeDelta::FromMilliseconds(kFadeOutMilliseconds);
    case OVERVIEW_ANIMATION_LAY_OUT_SELECTOR_ITEMS:
    case OVERVIEW_ANIMATION_RESTORE_WINDOW:
      return base::TimeDelta::FromMilliseconds(kTransitionMilliseconds);
    case OVERVIEW_ANIMATION_CLOSING_SELECTOR_ITEM:
      return base::TimeDelta::FromMilliseconds(kCloseScaleMilliseconds);
    case OVERVIEW_ANIMATION_CLOSE_SELECTOR_ITEM:
      return base::TimeDelta::FromMilliseconds(kCloseFadeOutMilliseconds);
  }
  NOTREACHED();
  return base::TimeDelta();
}

}  // namespace

ScopedOverviewAnimationSettingsAura::ScopedOverviewAnimationSettingsAura(
    OverviewAnimationType animation_type,
    aura::Window* window)
    : animation_settings_(new ui::ScopedLayerAnimationSettings(
          window->layer()->GetAnimator())) {
  switch (animation_type) {
    case OVERVIEW_ANIMATION_NONE:
      animation_settings_->SetPreemptionStrategy(
          ui::LayerAnimator::REPLACE_QUEUED_ANIMATIONS);
      break;
    case OVERVIEW_ANIMATION_ENTER_OVERVIEW_MODE_FADE_IN:
      animation_settings_->SetTweenType(gfx::Tween::EASE_IN);
      animation_settings_->SetPreemptionStrategy(
          ui::LayerAnimator::REPLACE_QUEUED_ANIMATIONS);
      break;
    case OVERVIEW_ANIMATION_EXIT_OVERVIEW_MODE_FADE_OUT:
      window->layer()->GetAnimator()->SchedulePauseForProperties(
          base::TimeDelta::FromMilliseconds(kFadeOutDelayMilliseconds),
          ui::LayerAnimationElement::OPACITY);
      animation_settings_->SetTweenType(gfx::Tween::EASE_OUT);
      animation_settings_->SetPreemptionStrategy(
          ui::LayerAnimator::REPLACE_QUEUED_ANIMATIONS);
      break;
    case OVERVIEW_ANIMATION_LAY_OUT_SELECTOR_ITEMS:
    case OVERVIEW_ANIMATION_RESTORE_WINDOW:
      animation_settings_->SetPreemptionStrategy(
          ui::LayerAnimator::IMMEDIATELY_ANIMATE_TO_NEW_TARGET);
      animation_settings_->SetTweenType(gfx::Tween::EASE_OUT);
      break;
    case OVERVIEW_ANIMATION_CLOSING_SELECTOR_ITEM:
    case OVERVIEW_ANIMATION_CLOSE_SELECTOR_ITEM:
      animation_settings_->SetPreemptionStrategy(
          ui::LayerAnimator::ENQUEUE_NEW_ANIMATION);
      animation_settings_->SetTweenType(gfx::Tween::EASE_OUT);
      break;
  }
  animation_settings_->SetTransitionDuration(
      GetAnimationDuration(animation_type));
}

ScopedOverviewAnimationSettingsAura::~ScopedOverviewAnimationSettingsAura() {}

void ScopedOverviewAnimationSettingsAura::AddObserver(
    ui::ImplicitAnimationObserver* observer) {
  animation_settings_->AddObserver(observer);
}

}  // namespace ash
