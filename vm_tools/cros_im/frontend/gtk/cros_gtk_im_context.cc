// Copyright 2021 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "frontend/gtk/cros_gtk_im_context.h"

#include <gdk/gdk.h>
#include <gdk/gdkwayland.h>
#include <cstring>
#include <iostream>
#include <utility>

namespace cros_im {
namespace gtk {

namespace {

////////////////////////////////////////////////////////////////////////////////
// GObject integration

struct CrosGtkIMContextClass {
  GtkIMContextClass parent_class;
};

G_DEFINE_DYNAMIC_TYPE(CrosGtkIMContext,
                      cros_gtk_im_context,
                      GTK_TYPE_IM_CONTEXT)

template <typename T>
CrosGtkIMContext* ToCrosGtkIMContext(T* obj) {
  return G_TYPE_CHECK_INSTANCE_CAST((obj), cros_gtk_im_context_get_type(),
                                    CrosGtkIMContext);
}

void cros_gtk_im_context_init(CrosGtkIMContext* context) {
  new (context) CrosGtkIMContext();
}

void DisposeCrosGtkIMContext(GObject* gobject) {
  ToCrosGtkIMContext(gobject)->~CrosGtkIMContext();
  G_OBJECT_CLASS(cros_gtk_im_context_parent_class)->dispose(gobject);
}

template <auto F, typename... Args>
auto Fwd(GtkIMContext* context, Args... args) {
  return (ToCrosGtkIMContext(context)->*F)(args...);
}

void cros_gtk_im_context_class_init(CrosGtkIMContextClass* klass) {
  GtkIMContextClass* im_context = GTK_IM_CONTEXT_CLASS(klass);
  GObjectClass* gobject = G_OBJECT_CLASS(klass);

  gobject->dispose = DisposeCrosGtkIMContext;
  im_context->set_client_window = Fwd<&CrosGtkIMContext::SetClientWindow>;
  im_context->get_preedit_string = Fwd<&CrosGtkIMContext::GetPreeditString>;
  im_context->filter_keypress = Fwd<&CrosGtkIMContext::FilterKeypress>;
  im_context->focus_in = Fwd<&CrosGtkIMContext::FocusIn>;
  im_context->focus_out = Fwd<&CrosGtkIMContext::FocusOut>;
  im_context->reset = Fwd<&CrosGtkIMContext::Reset>;
  im_context->set_cursor_location = Fwd<&CrosGtkIMContext::SetCursorLocation>;
  im_context->set_surrounding = Fwd<&CrosGtkIMContext::SetSurrounding>;
}

void cros_gtk_im_context_class_finalize(CrosGtkIMContextClass* klass) {}

// end of GObject integration
////////////////////////////////////////////////////////////////////////////////

wl_seat* GetSeat() {
  return gdk_wayland_seat_get_wl_seat(
      gdk_display_get_default_seat(gdk_display_get_default()));
}

PangoAttribute* ToPangoAttribute(const PreeditStyle& style) {
  PangoAttribute* attr = nullptr;
  // TODO(timloh): Work out how to best style pre-edit text. This code tries to
  // match Chrome, but some applications fail to distinguish the different types
  // of underline. Adjusting fg/bg colours may be more robust.
  switch (style.style) {
    // Chrome does not currently send DEFAULT, NONE, ACTIVE, INACTIVE.
    default:
    case ZWP_TEXT_INPUT_V1_PREEDIT_STYLE_HIGHLIGHT:
    case ZWP_TEXT_INPUT_V1_PREEDIT_STYLE_SELECTION:
      attr = pango_attr_underline_new(PANGO_UNDERLINE_DOUBLE);
      break;
    case ZWP_TEXT_INPUT_V1_PREEDIT_STYLE_UNDERLINE:
      attr = pango_attr_underline_new(PANGO_UNDERLINE_SINGLE);
      break;
    case ZWP_TEXT_INPUT_V1_PREEDIT_STYLE_INCORRECT:
      attr = pango_attr_underline_new(PANGO_UNDERLINE_ERROR);
      break;
  }
  attr->start_index = style.index;
  attr->end_index = style.index + style.length;
  return attr;
}

GtkWindow* GdkWindowToGtkWindow(GdkWindow* window) {
  if (!window)
    return nullptr;
  // TODO(timloh): This doesn't fully work yet (e.g. click the Open button in
  // gedit). From GTK 3.24.19 (bullseye), set_client_window is changed to
  // set_client_widget and this may no longer be necessary.
  GdkWindow* toplevel = gdk_window_get_toplevel(window);
  for (GList* list = gtk_window_list_toplevels(); list; list = list->next) {
    GtkWindow* gtk_window = GTK_WINDOW(list->data);
    if (gtk_widget_get_window(GTK_WIDGET(gtk_window)) == toplevel) {
      return gtk_window;
    }
  }
  g_warning("Failed to find GtkWindow matching GdkWindow.");
  return nullptr;
}

}  // namespace

void CrosGtkIMContext::RegisterType(GTypeModule* module) {
  cros_gtk_im_context_register_type(module);
}

CrosGtkIMContext* CrosGtkIMContext::Create() {
  return ToCrosGtkIMContext(
      g_object_new(cros_gtk_im_context_get_type(), nullptr));
}

CrosGtkIMContext::CrosGtkIMContext()
    : backend_observer_(this),
      backend_(std::make_unique<IMContextBackend>(&backend_observer_)) {}

CrosGtkIMContext::~CrosGtkIMContext() = default;

void CrosGtkIMContext::SetClientWindow(GdkWindow* window) {
  g_set_object(&window_, window);
  g_set_object(&gtk_window_, GdkWindowToGtkWindow(window));
}

void CrosGtkIMContext::GetPreeditString(char** preedit,
                                        PangoAttrList** styles,
                                        int* cursor_pos) {
  if (preedit)
    *preedit = g_strdup(preedit_.c_str());
  if (cursor_pos)
    *cursor_pos = g_utf8_strlen(preedit_.c_str(), preedit_cursor_pos_);
  if (styles) {
    *styles = pango_attr_list_new();
    for (const auto& style : preedit_styles_)
      pango_attr_list_insert(*styles, ToPangoAttribute(style));
  }
}

gboolean CrosGtkIMContext::FilterKeypress(GdkEventKey* event) {
  // The compositor sends us events directly so we generally don't need to do
  // anything here. It is possible for key events to race with input field
  // activation, in which case we will fail to send the key event to the IME.
  return false;
}

void CrosGtkIMContext::FocusIn() {
  if (!window_) {
    g_warning("Received focus_in event without active window.");
    return;
  }

  wl_surface* surface = gdk_wayland_window_get_wl_surface(window_);
  if (!surface) {
    g_warning("GdkWindow doesn't have an associated wl_surface.");
    return;
  }

  backend_->Activate(GetSeat(), surface);

  // TODO(timloh): Set content type.

  // TODO(timloh): Work out when else we need to call this.
  bool result = false;
  g_signal_emit_by_name(this, "retrieve-surrounding", &result);
  if (!result)
    g_warning("Failed to retrieve surrounding text.");
}

void CrosGtkIMContext::FocusOut() {
  backend_->Deactivate();
}

void CrosGtkIMContext::Reset() {
  backend_->Reset();
}

void CrosGtkIMContext::SetCursorLocation(GdkRectangle* area) {
  if (!window_)
    return;

  int offset_x = 0, offset_y = 0;
  gdk_window_get_origin(window_, &offset_x, &offset_y);

  backend_->SetCursorLocation(offset_x + area->x, offset_y + area->y,
                              area->width, area->height);
}

void CrosGtkIMContext::SetSurrounding(const char* text,
                                      int len,
                                      int cursor_index) {
  if (len == -1) {
    // Null-terminated
    backend_->SetSurrounding(text, cursor_index);
  } else {
    // Not necessarily null-terminated
    backend_->SetSurrounding(std::string(text, len).c_str(), cursor_index);
  }
}

CrosGtkIMContext::BackendObserver::BackendObserver(CrosGtkIMContext* context)
    : context_(context) {}

void CrosGtkIMContext::BackendObserver::SetPreedit(
    const std::string& preedit,
    int cursor,
    const std::vector<PreeditStyle>& styles) {
  bool was_empty = context_->preedit_.empty();
  context_->preedit_ = preedit;
  context_->preedit_cursor_pos_ = cursor;
  context_->preedit_styles_ = styles;
  if (was_empty && !preedit.empty())
    g_signal_emit_by_name(context_, "preedit-start");
  g_signal_emit_by_name(context_, "preedit-changed");
  if (!was_empty && preedit.empty())
    g_signal_emit_by_name(context_, "preedit-end");
}

void CrosGtkIMContext::BackendObserver::Commit(const std::string& text) {
  if (!context_->preedit_.empty()) {
    context_->preedit_.clear();
    context_->preedit_cursor_pos_ = 0;
    context_->preedit_styles_.clear();
    g_signal_emit_by_name(context_, "preedit-changed");
    g_signal_emit_by_name(context_, "preedit-end");
  }
  g_signal_emit_by_name(context_, "commit", text.c_str());
}

void CrosGtkIMContext::BackendObserver::KeySym(uint32_t keysym,
                                               KeyState state) {
  if (!context_->window_ || !context_->gtk_window_)
    return;

  // TODO(timloh): This implementation is incomplete. Take a a look at GTK's
  // gdkdevice-wayland.c when implementing it properly.

  GdkEvent* raw_event = gdk_event_new(
      state == KeyState::kPressed ? GDK_KEY_PRESS : GDK_KEY_RELEASE);

  GdkEventKey* event = reinterpret_cast<GdkEventKey*>(raw_event);
  // Ref is dropped in gdk_event_free.
  g_set_object(&event->window, context_->window_);
  event->send_event = true;
  event->time = GDK_CURRENT_TIME;
  event->state = 0;
  event->keyval = keysym;

  // These are "deprecated and should never be used" so we leave them empty.
  // We may have to revisit if we find apps relying on these.
  event->length = 0;
  event->string = nullptr;

  if (keysym == GDK_KEY_BackSpace) {
    event->hardware_keycode = 22;
  } else {
    event->hardware_keycode = 0;
  }

  event->group = 0;
  event->is_modifier = false;

  bool result;
  g_signal_emit_by_name(context_->gtk_window_, "key-press-event", event,
                        &result);
  if (!result)
    g_warning("Failed to send key press event.");

  gdk_event_free(raw_event);
}

}  // namespace gtk
}  // namespace cros_im
