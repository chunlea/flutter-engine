// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/shell/platform/linux/fl_keyboard_manager.h"

#include <array>
#include <cinttypes>
#include <memory>
#include <string>

#include "flutter/shell/platform/linux/fl_key_channel_responder.h"
#include "flutter/shell/platform/linux/fl_key_embedder_responder.h"
#include "flutter/shell/platform/linux/fl_keyboard_layout.h"
#include "flutter/shell/platform/linux/fl_keyboard_pending_event.h"
#include "flutter/shell/platform/linux/key_mapping.h"

// Turn on this flag to print complete layout data when switching IMEs. The data
// is used in unit tests.
#define DEBUG_PRINT_LAYOUT

/* Declarations of private classes */

G_DECLARE_FINAL_TYPE(FlKeyboardManagerUserData,
                     fl_keyboard_manager_user_data,
                     FL,
                     KEYBOARD_MANAGER_USER_DATA,
                     GObject);

/* End declarations */

namespace {

static bool is_eascii(uint16_t character) {
  return character < 256;
}

#ifdef DEBUG_PRINT_LAYOUT
// Prints layout entries that will be parsed by `MockLayoutData`.
void debug_format_layout_data(std::string& debug_layout_data,
                              uint16_t keycode,
                              uint16_t clue1,
                              uint16_t clue2) {
  if (keycode % 4 == 0) {
    debug_layout_data.append("    ");
  }

  constexpr int kBufferSize = 30;
  char buffer[kBufferSize];
  buffer[0] = 0;
  buffer[kBufferSize - 1] = 0;

  snprintf(buffer, kBufferSize, "0x%04x, 0x%04x, ", clue1, clue2);
  debug_layout_data.append(buffer);

  if (keycode % 4 == 3) {
    snprintf(buffer, kBufferSize, " // 0x%02x", keycode);
    debug_layout_data.append(buffer);
  }
}
#endif

}  // namespace

/* Define FlKeyboardManagerUserData */

/**
 * FlKeyboardManagerUserData:
 * The user_data used when #FlKeyboardManager sends event to
 * responders.
 */

struct _FlKeyboardManagerUserData {
  GObject parent_instance;

  // The owner manager.
  GWeakRef manager;
  uint64_t sequence_id;
};

G_DEFINE_TYPE(FlKeyboardManagerUserData,
              fl_keyboard_manager_user_data,
              G_TYPE_OBJECT)

static void fl_keyboard_manager_user_data_dispose(GObject* object) {
  g_return_if_fail(FL_IS_KEYBOARD_MANAGER_USER_DATA(object));
  FlKeyboardManagerUserData* self = FL_KEYBOARD_MANAGER_USER_DATA(object);

  g_weak_ref_clear(&self->manager);

  G_OBJECT_CLASS(fl_keyboard_manager_user_data_parent_class)->dispose(object);
}

static void fl_keyboard_manager_user_data_class_init(
    FlKeyboardManagerUserDataClass* klass) {
  G_OBJECT_CLASS(klass)->dispose = fl_keyboard_manager_user_data_dispose;
}

static void fl_keyboard_manager_user_data_init(
    FlKeyboardManagerUserData* self) {}

// Creates a new FlKeyboardManagerUserData private class with all information.
static FlKeyboardManagerUserData* fl_keyboard_manager_user_data_new(
    FlKeyboardManager* manager,
    uint64_t sequence_id) {
  FlKeyboardManagerUserData* self = FL_KEYBOARD_MANAGER_USER_DATA(
      g_object_new(fl_keyboard_manager_user_data_get_type(), nullptr));

  g_weak_ref_init(&self->manager, manager);
  self->sequence_id = sequence_id;
  return self;
}

/* Define FlKeyboardManager */

struct _FlKeyboardManager {
  GObject parent_instance;

  GWeakRef view_delegate;

  // An array of #FlKeyResponder. Elements are added with
  // #fl_keyboard_manager_add_responder immediately after initialization and are
  // automatically released on dispose.
  GPtrArray* responder_list;

  // An array of #FlKeyboardPendingEvent.
  //
  // Its elements are *not* unreferenced when removed. When FlKeyboardManager is
  // disposed, this array will be set with a free_func so that the elements are
  // unreferenced when removed.
  GPtrArray* pending_responds;

  // An array of #FlKeyboardPendingEvent.
  //
  // Its elements are unreferenced when removed.
  GPtrArray* pending_redispatches;

  // The last sequence ID used. Increased by 1 by every use.
  uint64_t last_sequence_id;

  // Record the derived layout.
  //
  // It is cleared when the platform reports a layout switch. Each entry,
  // which corresponds to a group, is only initialized on the arrival of the
  // first event for that group that has a goal keycode.
  FlKeyboardLayout* derived_layout;

  // A static map from keycodes to all layout goals.
  //
  // It is set up when the manager is initialized and is not changed ever after.
  std::unique_ptr<std::map<uint16_t, const LayoutGoal*>> keycode_to_goals;

  // A static map from logical keys to all mandatory layout goals.
  //
  // It is set up when the manager is initialized and is not changed ever after.
  std::unique_ptr<std::map<uint64_t, const LayoutGoal*>>
      logical_to_mandatory_goals;
};

G_DEFINE_TYPE(FlKeyboardManager, fl_keyboard_manager, G_TYPE_OBJECT);

// This is an exact copy of g_ptr_array_find_with_equal_func.  Somehow CI
// reports that can not find symbol g_ptr_array_find_with_equal_func, despite
// the fact that it runs well locally.
static gboolean g_ptr_array_find_with_equal_func1(GPtrArray* haystack,
                                                  gconstpointer needle,
                                                  GEqualFunc equal_func,
                                                  guint* index_) {
  guint i;
  g_return_val_if_fail(haystack != NULL, FALSE);
  if (equal_func == NULL) {
    equal_func = g_direct_equal;
  }
  for (i = 0; i < haystack->len; i++) {
    if (equal_func(g_ptr_array_index(haystack, i), needle)) {
      if (index_ != NULL) {
        *index_ = i;
      }
      return TRUE;
    }
  }

  return FALSE;
}

// Compare a #FlKeyboardPendingEvent with the given sequence_id.
static gboolean compare_pending_by_sequence_id(gconstpointer a,
                                               gconstpointer b) {
  FlKeyboardPendingEvent* pending =
      FL_KEYBOARD_PENDING_EVENT(const_cast<gpointer>(a));
  uint64_t sequence_id = *reinterpret_cast<const uint64_t*>(b);
  return fl_keyboard_pending_event_get_sequence_id(pending) == sequence_id;
}

// Compare a #FlKeyboardPendingEvent with the given hash.
static gboolean compare_pending_by_hash(gconstpointer a, gconstpointer b) {
  FlKeyboardPendingEvent* pending =
      FL_KEYBOARD_PENDING_EVENT(const_cast<gpointer>(a));
  uint64_t hash = *reinterpret_cast<const uint64_t*>(b);
  return fl_keyboard_pending_event_get_hash(pending) == hash;
}

// Try to remove a pending event from `pending_redispatches` with the target
// hash.
//
// Returns true if the event is found and removed.
static bool fl_keyboard_manager_remove_redispatched(FlKeyboardManager* self,
                                                    uint64_t hash) {
  guint result_index;
  gboolean found = g_ptr_array_find_with_equal_func1(
      self->pending_redispatches, static_cast<const uint64_t*>(&hash),
      compare_pending_by_hash, &result_index);
  if (found) {
    // The removed object is freed due to `pending_redispatches`'s free_func.
    g_ptr_array_remove_index_fast(self->pending_redispatches, result_index);
    return TRUE;
  } else {
    return FALSE;
  }
}

// The callback used by a responder after the event was dispatched.
static void responder_handle_event_callback(bool handled,
                                            gpointer user_data_ptr) {
  g_return_if_fail(FL_IS_KEYBOARD_MANAGER_USER_DATA(user_data_ptr));
  FlKeyboardManagerUserData* user_data =
      FL_KEYBOARD_MANAGER_USER_DATA(user_data_ptr);

  g_autoptr(FlKeyboardManager) self =
      FL_KEYBOARD_MANAGER(g_weak_ref_get(&user_data->manager));
  if (self == nullptr) {
    return;
  }

  g_autoptr(FlKeyboardViewDelegate) view_delegate =
      FL_KEYBOARD_VIEW_DELEGATE(g_weak_ref_get(&self->view_delegate));
  if (view_delegate == nullptr) {
    return;
  }

  guint result_index = -1;
  gboolean found = g_ptr_array_find_with_equal_func1(
      self->pending_responds, &user_data->sequence_id,
      compare_pending_by_sequence_id, &result_index);
  g_return_if_fail(found);
  FlKeyboardPendingEvent* pending = FL_KEYBOARD_PENDING_EVENT(
      g_ptr_array_index(self->pending_responds, result_index));
  g_return_if_fail(pending != nullptr);
  fl_keyboard_pending_event_mark_replied(pending, handled);
  // All responders have replied.
  if (fl_keyboard_pending_event_is_complete(pending)) {
    g_object_unref(user_data_ptr);
    gpointer removed =
        g_ptr_array_remove_index_fast(self->pending_responds, result_index);
    g_return_if_fail(removed == pending);
    bool should_redispatch =
        !fl_keyboard_pending_event_get_any_handled(pending) &&
        !fl_keyboard_view_delegate_text_filter_key_press(
            view_delegate, fl_keyboard_pending_event_get_event(pending));
    if (should_redispatch) {
      g_ptr_array_add(self->pending_redispatches, pending);
      fl_keyboard_view_delegate_redispatch_event(
          view_delegate,
          FL_KEY_EVENT(fl_keyboard_pending_event_get_event(pending)));
    } else {
      g_object_unref(pending);
    }
  }
}

static uint16_t convert_key_to_char(FlKeyboardViewDelegate* view_delegate,
                                    guint keycode,
                                    gint group,
                                    gint level) {
  GdkKeymapKey key = {keycode, group, level};
  constexpr int kBmpMax = 0xD7FF;
  guint origin = fl_keyboard_view_delegate_lookup_key(view_delegate, &key);
  return origin < kBmpMax ? origin : 0xFFFF;
}

// Make sure that Flutter has derived the layout for the group of the event,
// if the event contains a goal keycode.
static void guarantee_layout(FlKeyboardManager* self, FlKeyEvent* event) {
  g_autoptr(FlKeyboardViewDelegate) view_delegate =
      FL_KEYBOARD_VIEW_DELEGATE(g_weak_ref_get(&self->view_delegate));
  if (view_delegate == nullptr) {
    return;
  }

  guint8 group = fl_key_event_get_group(event);
  if (fl_keyboard_layout_has_group(self->derived_layout, group)) {
    return;
  }
  if (self->keycode_to_goals->find(fl_key_event_get_keycode(event)) ==
      self->keycode_to_goals->end()) {
    return;
  }

  // Clone all mandatory goals. Each goal is removed from this cloned map when
  // fulfilled, and the remaining ones will be assigned to a default position.
  std::map<uint64_t, const LayoutGoal*> remaining_mandatory_goals =
      *self->logical_to_mandatory_goals;

#ifdef DEBUG_PRINT_LAYOUT
  std::string debug_layout_data;
  for (uint16_t keycode = 0; keycode < 128; keycode += 1) {
    std::vector<uint16_t> this_key_clues = {
        convert_key_to_char(view_delegate, keycode, group, 0),
        convert_key_to_char(view_delegate, keycode, group, 1),  // Shift
    };
    debug_format_layout_data(debug_layout_data, keycode, this_key_clues[0],
                             this_key_clues[1]);
  }
#endif

  // It's important to only traverse layout goals instead of all keycodes.
  // Some key codes outside of the standard keyboard also gives alpha-numeric
  // letters, and will therefore take over mandatory goals from standard
  // keyboard keys if they come first. Example: French keyboard digit 1.
  for (const LayoutGoal& keycode_goal : layout_goals) {
    uint16_t keycode = keycode_goal.keycode;
    std::vector<uint16_t> this_key_clues = {
        convert_key_to_char(view_delegate, keycode, group, 0),
        convert_key_to_char(view_delegate, keycode, group, 1),  // Shift
    };

    // The logical key should be the first available clue from below:
    //
    //  - Mandatory goal, if it matches any clue. This ensures that all alnum
    //    keys can be found somewhere.
    //  - US layout, if neither clue of the key is EASCII. This ensures that
    //    there are no non-latin logical keys.
    //  - A value derived on the fly from keycode & keyval.
    for (uint16_t clue : this_key_clues) {
      auto matching_goal = remaining_mandatory_goals.find(clue);
      if (matching_goal != remaining_mandatory_goals.end()) {
        // Found a key that produces a mandatory char. Use it.
        g_return_if_fail(fl_keyboard_layout_get_logical_key(
                             self->derived_layout, group, keycode) == 0);
        fl_keyboard_layout_set_logical_key(self->derived_layout, group, keycode,
                                           clue);
        remaining_mandatory_goals.erase(matching_goal);
        break;
      }
    }
    bool has_any_eascii =
        is_eascii(this_key_clues[0]) || is_eascii(this_key_clues[1]);
    // See if any produced char meets the requirement as a logical key.
    if (fl_keyboard_layout_get_logical_key(self->derived_layout, group,
                                           keycode) == 0 &&
        !has_any_eascii) {
      auto found_us_layout = self->keycode_to_goals->find(keycode);
      if (found_us_layout != self->keycode_to_goals->end()) {
        fl_keyboard_layout_set_logical_key(
            self->derived_layout, group, keycode,
            found_us_layout->second->logical_key);
      }
    }
  }

  // Ensure all mandatory goals are assigned.
  for (const auto mandatory_goal_iter : remaining_mandatory_goals) {
    const LayoutGoal* goal = mandatory_goal_iter.second;
    fl_keyboard_layout_set_logical_key(self->derived_layout, group,
                                       goal->keycode, goal->logical_key);
  }
}

static void fl_keyboard_manager_dispose(GObject* object) {
  FlKeyboardManager* self = FL_KEYBOARD_MANAGER(object);

  g_weak_ref_clear(&self->view_delegate);

  self->keycode_to_goals.reset();
  self->logical_to_mandatory_goals.reset();

  g_ptr_array_free(self->responder_list, TRUE);
  g_ptr_array_set_free_func(self->pending_responds, g_object_unref);
  g_ptr_array_free(self->pending_responds, TRUE);
  g_ptr_array_free(self->pending_redispatches, TRUE);
  g_clear_object(&self->derived_layout);

  G_OBJECT_CLASS(fl_keyboard_manager_parent_class)->dispose(object);
}

static void fl_keyboard_manager_class_init(FlKeyboardManagerClass* klass) {
  G_OBJECT_CLASS(klass)->dispose = fl_keyboard_manager_dispose;
}

static void fl_keyboard_manager_init(FlKeyboardManager* self) {
  self->derived_layout = fl_keyboard_layout_new();

  self->keycode_to_goals =
      std::make_unique<std::map<uint16_t, const LayoutGoal*>>();
  self->logical_to_mandatory_goals =
      std::make_unique<std::map<uint64_t, const LayoutGoal*>>();
  for (const LayoutGoal& goal : layout_goals) {
    (*self->keycode_to_goals)[goal.keycode] = &goal;
    if (goal.mandatory) {
      (*self->logical_to_mandatory_goals)[goal.logical_key] = &goal;
    }
  }

  self->responder_list = g_ptr_array_new_with_free_func(g_object_unref);

  self->pending_responds = g_ptr_array_new();
  self->pending_redispatches = g_ptr_array_new_with_free_func(g_object_unref);

  self->last_sequence_id = 1;
}

FlKeyboardManager* fl_keyboard_manager_new(
    FlBinaryMessenger* messenger,
    FlKeyboardViewDelegate* view_delegate) {
  g_return_val_if_fail(FL_IS_KEYBOARD_VIEW_DELEGATE(view_delegate), nullptr);

  FlKeyboardManager* self = FL_KEYBOARD_MANAGER(
      g_object_new(fl_keyboard_manager_get_type(), nullptr));

  g_weak_ref_init(&self->view_delegate, view_delegate);

  // The embedder responder must be added before the channel responder.
  g_ptr_array_add(
      self->responder_list,
      FL_KEY_RESPONDER(fl_key_embedder_responder_new(
          [](const FlutterKeyEvent* event, FlutterKeyEventCallback callback,
             void* callback_user_data, void* send_key_event_user_data) {
            FlKeyboardManager* self =
                FL_KEYBOARD_MANAGER(send_key_event_user_data);
            g_autoptr(FlKeyboardViewDelegate) view_delegate =
                FL_KEYBOARD_VIEW_DELEGATE(g_weak_ref_get(&self->view_delegate));
            if (view_delegate == nullptr) {
              return;
            }
            fl_keyboard_view_delegate_send_key_event(
                view_delegate, event, callback, callback_user_data);
          },
          self)));
  g_ptr_array_add(self->responder_list,
                  FL_KEY_RESPONDER(fl_key_channel_responder_new(messenger)));

  return self;
}

gboolean fl_keyboard_manager_handle_event(FlKeyboardManager* self,
                                          FlKeyEvent* event) {
  g_return_val_if_fail(FL_IS_KEYBOARD_MANAGER(self), FALSE);
  g_return_val_if_fail(event != nullptr, FALSE);

  guarantee_layout(self, event);

  uint64_t incoming_hash = fl_key_event_hash(event);
  if (fl_keyboard_manager_remove_redispatched(self, incoming_hash)) {
    return FALSE;
  }

  FlKeyboardPendingEvent* pending = fl_keyboard_pending_event_new(
      event, ++self->last_sequence_id, self->responder_list->len);

  g_ptr_array_add(self->pending_responds, pending);
  FlKeyboardManagerUserData* user_data = fl_keyboard_manager_user_data_new(
      self, fl_keyboard_pending_event_get_sequence_id(pending));
  uint64_t specified_logical_key = fl_keyboard_layout_get_logical_key(
      self->derived_layout, fl_key_event_get_group(event),
      fl_key_event_get_keycode(event));
  for (guint i = 0; i < self->responder_list->len; i++) {
    FlKeyResponder* responder =
        FL_KEY_RESPONDER(g_ptr_array_index(self->responder_list, i));
    fl_key_responder_handle_event(responder, event,
                                  responder_handle_event_callback, user_data,
                                  specified_logical_key);
  }

  return TRUE;
}

gboolean fl_keyboard_manager_is_state_clear(FlKeyboardManager* self) {
  g_return_val_if_fail(FL_IS_KEYBOARD_MANAGER(self), FALSE);
  return self->pending_responds->len == 0 &&
         self->pending_redispatches->len == 0;
}

void fl_keyboard_manager_sync_modifier_if_needed(FlKeyboardManager* self,
                                                 guint state,
                                                 double event_time) {
  g_return_if_fail(FL_IS_KEYBOARD_MANAGER(self));

  // The embedder responder is the first element in
  // FlKeyboardManager.responder_list.
  FlKeyEmbedderResponder* responder =
      FL_KEY_EMBEDDER_RESPONDER(g_ptr_array_index(self->responder_list, 0));
  fl_key_embedder_responder_sync_modifiers_if_needed(responder, state,
                                                     event_time);
}

GHashTable* fl_keyboard_manager_get_pressed_state(FlKeyboardManager* self) {
  g_return_val_if_fail(FL_IS_KEYBOARD_MANAGER(self), nullptr);

  // The embedder responder is the first element in
  // FlKeyboardManager.responder_list.
  FlKeyEmbedderResponder* responder =
      FL_KEY_EMBEDDER_RESPONDER(g_ptr_array_index(self->responder_list, 0));
  return fl_key_embedder_responder_get_pressed_state(responder);
}

void fl_keyboard_manager_notify_layout_changed(FlKeyboardManager* self) {
  g_return_if_fail(FL_IS_KEYBOARD_MANAGER(self));
  g_clear_object(&self->derived_layout);
  self->derived_layout = fl_keyboard_layout_new();
}
