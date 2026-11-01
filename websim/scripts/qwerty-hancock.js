/*
 * Qwerty Hancock keyboard library v0.11.0
 * The web keyboard for now people.
 * Copyright 2012-20, Stuart Memo
 *
 * Licensed under the MIT License
 * http://opensource.org/licenses/mit-license.php
 *
 * http://stuartmemo.com/qwerty-hancock
 */

(function () {
  var root = this;
  /* In <script> context, `this` is the window.
   * In node context (browserify), `this` is the node global.
   */
  var globalWindow = typeof global === 'undefined' ? root : root.window;
  var version = '0.11.0',
    settings = {},
    mouse_is_down = false,
    keysDown = {},
    key_map = {
      65: 'Cl',
      87: 'C#l',
      83: 'Dl',
      69: 'D#l',
      68: 'El',
      70: 'Fl',
      84: 'F#l',
      71: 'Gl',
      89: 'G#l',
      // 90: 'G#l',
      72: 'Al',
      85: 'A#l',
      74: 'Bl',
      75: 'Cu',
      79: 'C#u',
      76: 'Du',
      80: 'D#u',
      59: 'Eu',
      186: 'Eu',
      222: 'Fu',
      221: 'F#u',
      220: 'Gu',
    },
    keyDownCallback,
    keyUpCallback;

  /**
   * Calculate width of white key.
   * @return {number} Width of a single white key in pixels.
   */
  var getWhiteKeyWidth = function (number_of_white_keys) {
    return Math.floor(
      (settings.width - number_of_white_keys) / number_of_white_keys
    );
  };

  /**
   * Merge user settings with defaults.
   * @param  {object} user_settings
   */
  var init = function (us) {
    var container;

    user_settings = us || {};

    settings = {
      id: user_settings.id || 'keyboard',
      octaves: user_settings.octaves || 3,
      width: user_settings.width,
      height: user_settings.height,
      margin: user_settings.margin || 0,
      startNote: user_settings.startNote || 'A3',
      keyMap: user_settings.keyMap,
      whiteKeyColour: user_settings.whiteKeyColour || '#fff',
      blackKeyColour: user_settings.blackKeyColour || '#000',
      activeColour: user_settings.activeColour || 'yellow',
      borderColour: user_settings.borderColour || '#000',
      keyboardLayout: user_settings.keyboardLayout || 'en',
      musicalTyping: user_settings.musicalTyping === false ? false : true,
    };

    container = document.getElementById(settings.id);
    if (!container) {
      container = document.createElement('div');
      container.id = settings.id;
      document.body.appendChild(container);
    }

    // Get computed style to ensure we have the real width
    var computedStyle = window.getComputedStyle(container);

    // When the caller doesn't pin a width (e.g. the drone sim's Oscillator
    // keyboard), let the container keep its CSS-driven width (w-full) and
    // only read the computed value once for key layout. When width IS
    // provided (fx.html / xypad.html), keep the old behaviour: write it
    // back as an inline style so the box is exactly that size.
    var widthExplicit = typeof user_settings.width !== 'undefined';
    settings._explicitWidth = widthExplicit;

    if (!widthExplicit) {
      settings.width = parseInt(computedStyle.width) || 600;
    }

    if (typeof settings.height === 'undefined') {
      settings.height = parseInt(computedStyle.height) || 150;
    }

    // Set container dimensions explicitly (height always; width only when
    // the caller asked for a fixed size - otherwise the CSS class rules).
    if (widthExplicit) {
      container.style.width = settings.width + 'px';
    }
    container.style.height = settings.height + 'px';

    // Set container display to block to ensure dimensions are applied
    container.style.display = 'block';
    container.style.position = 'relative';
    container.style.boxSizing = 'content-box';

    settings.startOctave = parseInt(settings.startNote.charAt(1), 10);
    settings.keyOctave = user_settings.keyOctave || settings.startOctave;

    // Explicit keyMap mode: settings.keyMap = {keyCode: "NoteName+Octave"}
    // (e.g. {65: "A2", 87: "A#2"}). Keys are created in keyMap order, notes
    // with '#' are black, the rest white. setKeyOctave() then acts as an
    // octave *shift* applied to every key (relabelKeys rewrites the ids).
    // The classic l/u suffix path (fx.html / xypad.html) is untouched.
    settings.customKeyMap = !!(
      settings.keyMap && Object.keys(settings.keyMap).length > 0
    );
    settings.octaveShift = settings.customKeyMap
      ? user_settings.keyOctave || 0
      : 0;
    if (settings.customKeyMap) {
      settings.whiteKeyCount = Object.keys(settings.keyMap).filter(function (
        code
      ) {
        return settings.keyMap[code].replace(/\d+$/, '').indexOf('#') === -1;
      }).length;
    }

    // Add getters and setters
    this.setKeyOctave = function (octave) {
      if (settings.customKeyMap) {
        settings.octaveShift = octave;
        relabelKeys();
        return settings.octaveShift;
      }
      settings.keyOctave = octave;
      return settings.keyOctave;
    };
    this.getKeyOctave = function () {
      return settings.customKeyMap ? settings.octaveShift : settings.keyOctave;
    };
    this.keyOctaveUp = function () {
      if (settings.customKeyMap) {
        settings.octaveShift++;
        relabelKeys();
        return settings.octaveShift;
      }
      settings.keyOctave++;
      return settings.keyOctave;
    };
    this.keyOctaveDown = function () {
      if (settings.customKeyMap) {
        settings.octaveShift--;
        relabelKeys();
        return settings.octaveShift;
      }
      settings.keyOctave--;
      return settings.keyOctave;
    };
    this.getKeyMap = function () {
      return key_map;
    };
    this.setKeyMap = function (newKeyMap) {
      key_map = newKeyMap;
      return key_map;
    };

    createKeyboard();
    addListeners.call(this, container);

    // For fluid-width keyboards (no explicit `width` in the config), track
    // the container and re-layout the keys when its width changes - e.g.
    // when the Oscillator card resizes with the viewport or the right
    // column collapses on mobile. Explicit-width instances (fx.html /
    // xypad.html) keep the legacy fixed-size behaviour.
    if (!settings._explicitWidth && typeof ResizeObserver !== 'undefined') {
      new ResizeObserver(function () {
        reflowKeyboard();
      }).observe(container);
    }
  };

  /**
   * Get key number of a given note.
   * @param  {string} note Musical note to convert into key number.
   * @return {number} Key number of note.
   */
  var getKeyNumber = function (note) {
    var notes = [
        'A',
        'A#',
        'B',
        'C',
        'C#',
        'D',
        'D#',
        'E',
        'F',
        'F#',
        'G',
        'G#',
      ],
      key_number,
      octave;

    if (note.length === 3) {
      octave = note.charAt(2);
    } else {
      octave = note.charAt(1);
    }

    key_number = notes.indexOf(note.slice(0, -1));

    if (key_number < 3) {
      key_number = key_number + 12 + (octave - 1) * 12 + 1;
    } else {
      key_number = key_number + (octave - 1) * 12 + 1;
    }

    return key_number;
  };

  /**
   * Get frequency of a given note.
   * @param  {string} note Musical note to convert into hertz.
   * @return {number} Frequency of note in hertz.
   */
  var getFrequencyOfNote = function (note) {
    var key_number = getKeyNumber(note);
    return 440 * Math.pow(2, (key_number - 49) / 12);
  };

  /**
   * Get MIDI note number of a given note.
   * @param  {string} note Musical note to convert into MIDI note number.
   * @return {number} MIDI note number.
   */
  var getMidiNoteNumber = function (note) {
    return getKeyNumber(note) + 20;
  };

  /**
   * Lighten up man. Change the colour of a key.
   * @param  {element} el DOM element to change colour of.
   */
  var lightenUp = function lightenUp(el) {
    if (el !== null && typeof el !== 'undefined') {
      el.style.backgroundColor = settings.activeColour;
    }
  };

  /**
   * Revert key to original colour.
   * @param  {element} el DOM element to change colour of.
   */
  var darkenDown = function darkenDown(el) {
    if (el !== null) {
      if (el.getAttribute('data-note-type') === 'white') {
        el.style.backgroundColor = settings.whiteKeyColour;
      } else {
        el.style.backgroundColor = settings.blackKeyColour;
      }
    }
  };

  /**
   * Order notes into order defined by starting key in settings.
   * @param {array} notes_to_order Notes to be ordered.
   * @return {array{ ordered_notes Ordered notes.
   */
  var orderNotes = function (notes_to_order) {
    var i,
      keyOffset = 0,
      number_of_notes_to_order = notes_to_order.length,
      ordered_notes = [];

    for (i = 0; i < number_of_notes_to_order; i++) {
      if (settings.startNote.charAt(0) === notes_to_order[i]) {
        keyOffset = i;
        break;
      }
    }

    for (i = 0; i < number_of_notes_to_order; i++) {
      if (i + keyOffset > number_of_notes_to_order - 1) {
        ordered_notes[i] =
          notes_to_order[i + keyOffset - number_of_notes_to_order];
      } else {
        ordered_notes[i] = notes_to_order[i + keyOffset];
      }
    }

    return ordered_notes;
  };

  /**
   * Add styling to individual white key.
   * @param  {element} el White key DOM element.
   */
  var styleWhiteKey = function (key) {
    key.el.style.backgroundColor = settings.whiteKeyColour;
    key.el.style.border = '1px solid ' + settings.borderColour;
    key.el.style.borderRight = 0;
    key.el.style.height = settings.height + 'px';
    key.el.style.width = key.width + 'px';
    key.el.style.borderRadius = '0 0 5px 5px';
    key.el.style.position = 'relative';
    key.el.style.zIndex = '1';
    key.el.style.boxSizing = 'content-box';

    if (key.noteNumber === getTotalWhiteKeys() - 1) {
      key.el.style.border = '1px solid ' + settings.borderColour;
    }
  };

  /**
   * Add styling to individual black key.
   * @param  {element} el Black key DOM element.
   */
  var styleBlackKey = function (key) {
    var white_key_width = getWhiteKeyWidth(getTotalWhiteKeys()),
      black_key_width = Math.floor(white_key_width / 2);

    key.el.style.backgroundColor = settings.blackKeyColour;
    key.el.style.border = '1px solid ' + settings.borderColour;
    key.el.style.position = 'absolute';
    key.el.style.left =
      Math.floor(
        (white_key_width + 1) * (key.noteNumber + 1) - black_key_width / 2
      ) + 'px';
    key.el.style.width = black_key_width + 'px';
    key.el.style.height = settings.height / 1.5 + 'px';
    key.el.style.borderRadius = '0 0 3px 3px';
    key.el.style.zIndex = '2';
    key.el.style.boxSizing = 'content-box';
  };

  /**
   * Rewrite the octave in every key id after an octave shift (custom keyMap
   * mode only) and reposition the black keys, since their absolute `left`
   * depends on the white key count.
   */
  var relabelKeys = function () {
    if (!settings.customKeyMap) return;
    var white_key_width = getWhiteKeyWidth(getTotalWhiteKeys());
    var black_key_width = Math.floor(white_key_width / 2);
    var ul = document.getElementById(settings.id).querySelector('ul');
    if (!ul) return;
    Array.prototype.slice.call(ul.children).forEach(function (el) {
      // data-base holds the unshifted note id (e.g. "A#3") stored at key
      // creation time. Deriving the new id from it (instead of from the
      // current, already-shifted id) means repeated relabel calls cannot
      // accumulate the shift. Using data-base also sidesteps the ambiguity
      // of note names: A#2 and A#3 both parse to "A#", but their data-base
      // values are distinct.
      var base = el.getAttribute('data-base');
      if (!base) return;
      var m = base.match(/^([A-G]#?)(\d+)$/);
      if (!m) return;
      var newId = m[1] + (parseInt(m[2], 10) + settings.octaveShift);
      el.id = newId;
      el.title = newId;
      if (el.getAttribute('data-note-type') === 'black') {
        el.style.left =
          Math.floor(
            (white_key_width + 1) * (parseInt(el.dataset.noteNumber, 10) + 1) -
              black_key_width / 2
          ) + 'px';
      }
    });
  };

  /**
   * Add styling to individual key on keyboard.
   * @param  {object} key Element of key.
   */
  var styleKey = function (key) {
    key.el.style['-webkit-user-select'] = 'none';

    if (key.colour === 'white') {
      // White keys are flex items of the <ul> (see styleKeyboard): they line
      // up in one row and never wrap, regardless of how wide the container
      // is. display:inline-block (the library default) would wrap once the
      // computed key width no longer fits, which breaks the piano layout.
      key.el.style.display = 'block';
      styleWhiteKey(key);
    } else {
      // Black keys stay absolute (positioned by noteNumber) and overlay the
      // white row - their layout is independent of the flex flow.
      styleBlackKey(key);
    }
  };

  /**
   * Reset styles on keyboard container and list element.
   * @param {element} keyboard Keyboard container DOM element.
   */
  var styleKeyboard = function (keyboard) {
    var styleElement = function (el) {
      el.style.cursor = 'default';
      el.style.fontSize = '0px';
      el.style.height = settings.height + 'px';
      el.style.padding = 0;
      el.style.position = 'relative';
      el.style.listStyle = 'none';
      el.style.margin = settings.margin;
      el.style['-webkit-user-select'] = 'none';
      el.style.boxSizing = 'content-box';
    };

    styleElement(keyboard.container);
    styleElement(keyboard.el);
    // The <ul> is a flex row of white keys (black keys are absolute, so they
    // don't participate in the flex flow). Only pin an explicit width when
    // the caller asked for one; otherwise the container's CSS width (e.g.
    // w-full) wins, so the keyboard resizes with its parent card.
    keyboard.el.style.display = 'flex';
    keyboard.el.style.flexWrap = 'nowrap';
    if (settings._explicitWidth) {
      keyboard.el.style.width = settings.width + 'px';
    } else {
      keyboard.el.style.width = '100%';
    }
  };

  /**
   * Re-layout every key after the container's width has changed. Used by the
   * ResizeObserver installed for non-explicit-width instances (the drone
   * sim's w-full keyboard): settings.width is refreshed from the container,
   * then each key's width (white) or left+width (black) is recomputed in
   * place. The DOM is not rebuilt - only inline styles are rewritten - so
   * active keys, listeners and octave shift state all survive.
   */
  var reflowKeyboard = function () {
    var container = document.getElementById(settings.id);
    if (!container) return;
    var newWidth = container.getBoundingClientRect().width;
    if (!newWidth || Math.abs(newWidth - settings.width) < 1) return;
    settings.width = Math.floor(newWidth);

    var white_key_width = getWhiteKeyWidth(getTotalWhiteKeys());
    var black_key_width = Math.floor(white_key_width / 2);
    var ul = container.querySelector('ul');
    if (!ul) return;

    Array.prototype.slice.call(ul.children).forEach(function (el) {
      if (el.getAttribute('data-note-type') === 'white') {
        el.style.width = white_key_width + 'px';
      } else {
        el.style.width = black_key_width + 'px';
        el.style.left =
          Math.floor(
            (white_key_width + 1) * (parseInt(el.dataset.noteNumber, 10) + 1) -
              black_key_width / 2
          ) + 'px';
      }
    });
  };

  /**
   * Call user's mouseDown event.
   */
  var mouseDown = function (element, callback) {
    if (element.tagName.toLowerCase() == 'li') {
      mouse_is_down = true;
      lightenUp(element);
      callback(
        getMidiNoteNumber(element.title),
        getFrequencyOfNote(element.title)
      );
    }
  };

  /**
   * Call user's mouseUp event.
   */
  var mouseUp = function (element, callback) {
    if (element.tagName.toLowerCase() == 'li') {
      mouse_is_down = false;
      darkenDown(element);
      callback(
        getMidiNoteNumber(element.title),
        getFrequencyOfNote(element.title)
      );
    }
  };

  /**
   * Call user's mouseDown if required.
   */
  var mouseOver = function (element, callback) {
    if (mouse_is_down) {
      lightenUp(element);
      callback(
        getMidiNoteNumber(element.title),
        getFrequencyOfNote(element.title)
      );
    }
  };

  /**
   * Call user's mouseUp if required.
   */
  var mouseOut = function (element, callback) {
    if (mouse_is_down) {
      darkenDown(element);
      callback(
        getMidiNoteNumber(element.title),
        getFrequencyOfNote(element.title)
      );
    }
  };

  /**
   * Create key DOM element.
   * @return {object} Key DOM element.
   */
  var createKey = function (key) {
    key.el = document.createElement('li');
    key.el.id = key.id;
    key.el.title = key.id;
    key.el.setAttribute('data-note-type', key.colour);
    key.el.setAttribute('data-note-number', key.noteNumber);
    // In custom keyMap mode, remember the unshifted (base) note id so
    // relabelKeys can re-derive the octave without scanning settings.keyMap
    // by note name (note names are not unique: A#2 and A#3 both parse to "A#").
    if (key.baseId) {
      key.el.setAttribute('data-base', key.baseId);
    }

    styleKey(key);

    return key;
  };

  var getTotalWhiteKeys = function () {
    if (settings.customKeyMap) return settings.whiteKeyCount;
    return settings.octaves * 7;
  };

  /**
   * Build a key from a note id like "C#4" (the custom keyMap path).
   * Octave is parsed from the id itself, so relabeling on octave shift only
   * needs to rewrite the trailing digit.
   * @param {string} noteName e.g. "A#", "B", "C#"
   * @param {number} baseOctave
   * @param {number} noteNumber index among white keys (position for black keys)
   * @return {object} key object for createKey()
   */
  var makeCustomKey = function (noteName, baseOctave, noteNumber) {
    var id = noteName + (baseOctave + settings.octaveShift);
    return {
      baseId: noteName + baseOctave,
      colour: noteName.charAt(noteName.length - 1) === '#' ? 'black' : 'white',
      octave: baseOctave,
      width: getWhiteKeyWidth(getTotalWhiteKeys()),
      id: id,
      noteNumber: noteNumber,
    };
  };

  var createKeys = function () {
    var that = this,
      i,
      key,
      keys = [],
      note_counter = 0,
      octave_counter = settings.startOctave,
      total_white_keys = getTotalWhiteKeys();

    // Custom keyMap path: keys are given explicitly as {keyCode: "Note+Oct"}
    // (e.g. {65: "A2", 87: "A#2", ...}). Note names containing '#' are
    // rendered as black keys, everything else as white. The base octave is
    // taken from the note id and offset by settings.octaveShift (set via
    // setKeyOctave, e.g. by Z/X).
    //
    // The VISUAL order of keys on the web keyboard is always the musical
    // order (ascending MIDI), regardless of keyCode order. The keyCode
    // mapping only determines which physical key plays which note.
    //
    // NOTE: Object.keys() on an object with integer-like keys iterates in
    // ascending numeric order (ECMAScript spec), so we collect all notes and
    // sort them by their MIDI number to get the correct musical layout.
    if (settings.customKeyMap) {
      var notes = Object.keys(settings.keyMap).map(function (code) {
        var note = settings.keyMap[code];
        return {
          code: parseInt(code, 10),
          note: note,
          name: note.replace(/\d+$/, ''),
          baseOctave: parseInt(note.slice(-1), 10),
          midi: getKeyNumber(note)
        };
      }).sort(function (a, b) {
        return a.midi - b.midi;
      });
      var white_position = 0;
      notes.forEach(function (entry) {
        var isBlack = entry.name.charAt(entry.name.length - 1) === '#';
        // Black keys sit on the boundary after the previous white key, so
        // they share that white key's position index (same as the classic
        // path, where a black key gets noteNumber = i of the white it
        // follows). white_position is already incremented past the
        // previous white, so the previous white is at white_position - 1.
        var pos = isBlack ? white_position - 1 : white_position;
        var k = createKey(
          makeCustomKey(entry.name, entry.baseOctave, pos)
        );
        if (!isBlack) white_position++;
        keys.push(k.el);
      });
      return {
        keys: keys,
        totalWhiteKeys: settings.whiteKeyCount,
      };
    }

    for (i = 0; i < total_white_keys; i++) {
      if (i % this.whiteNotes.length === 0) {
        note_counter = 0;
      }

      bizarre_note_counter = this.whiteNotes[note_counter];

      if (bizarre_note_counter === 'C' && i !== 0) {
        octave_counter++;
      }

      key = createKey({
        colour: 'white',
        octave: octave_counter,
        width: getWhiteKeyWidth(total_white_keys),
        id: this.whiteNotes[note_counter] + octave_counter,
        noteNumber: i,
      });

      keys.push(key.el);

      if (i !== total_white_keys - 1) {
        this.notesWithSharps.forEach(function (note, index) {
          if (note === that.whiteNotes[note_counter]) {
            key = createKey({
              colour: 'black',
              octave: octave_counter,
              width: getWhiteKeyWidth(total_white_keys) / 2,
              id: that.whiteNotes[note_counter] + '#' + octave_counter,
              noteNumber: i,
            });

            keys.push(key.el);
          }
        });
      }
      note_counter++;
    }

    return {
      keys: keys,
      totalWhiteKeys: total_white_keys,
    };
  };

  var addKeysToKeyboard = function (keyboard) {
    keyboard.keys.forEach(function (key) {
      keyboard.el.appendChild(key);
    });
  };

  var setKeyPressOffset = function (sorted_white_notes) {
    settings.keyPressOffset = sorted_white_notes[0] === 'C' ? 0 : 1;
  };

  var createKeyboard = function () {
    var keyboard = {
      container: document.getElementById(settings.id),
      el: document.createElement('ul'),
      whiteNotes: orderNotes(['C', 'D', 'E', 'F', 'G', 'A', 'B']),
      notesWithSharps: orderNotes(['C', 'D', 'F', 'G', 'A']),
    };

    var keysObj = createKeys.call(keyboard);
    keyboard.keys = keysObj.keys;
    keyboard.totalWhiteKeys = keysObj.totalWhiteKeys;

    setKeyPressOffset(keyboard.whiteNotes);
    styleKeyboard(keyboard);

    // Add keys to keyboard, and keyboard to container.
    addKeysToKeyboard(keyboard);

    if (keyboard.container.querySelector('ul')) {
      keyboard.container.replaceChild(
        keyboard.el,
        keyboard.container.querySelector('ul')
      );
    } else {
      keyboard.container.appendChild(keyboard.el);
    }

    return keyboard;
  };

  var getKeyPressed = function (keyCode) {
    if (settings.customKeyMap) {
      // Custom keyMap: the note id already contains its base octave, so just
      // apply the octave shift.
      var note = settings.keyMap[keyCode];
      if (!note) return null;
      var m = note.match(/^(.*?)(\d+)$/);
      if (!m) return note;
      return m[1] + (parseInt(m[2], 10) + settings.octaveShift);
    }
    return key_map[keyCode]
      .replace('l', parseInt(settings.keyOctave, 10) + settings.keyPressOffset)
      .replace(
        'u',
        (
          parseInt(settings.keyOctave, 10) +
          settings.keyPressOffset +
          1
        ).toString()
      );
  };

  /**
   * Handle a keyboard key being pressed.
   * @param {object} key The keyboard event of the currently pressed key.
   * @param {callback} callback The user's noteDown function.
   * @return {boolean} true if it was a key (combo) used by qwerty-hancock
   */
  var keyboardDown = function (key, callback) {
    var key_pressed;

    if (key.keyCode in keysDown) {
      return false;
    }

    keysDown[key.keyCode] = true;

    if (typeof key_map[key.keyCode] !== 'undefined') {
      key_pressed = getKeyPressed(key.keyCode);

      // Call user's noteDown function.
      callback(getMidiNoteNumber(key_pressed), getFrequencyOfNote(key_pressed));
      lightenUp(document.getElementById(key_pressed));
      return true;
    }
    return false;
  };

  /**
   * Handle a keyboard key being released.
   * @param {element} key The DOM element of the key that was released.
   * @param {callback} callback The user's noteDown function.
   * @return {boolean} true if it was a key (combo) used by qwerty-hancock
   */
  var keyboardUp = function (key, callback) {
    var key_pressed;

    delete keysDown[key.keyCode];

    if (typeof key_map[key.keyCode] !== 'undefined') {
      key_pressed = getKeyPressed(key.keyCode);
      // Call user's noteDown function.
      callback(getMidiNoteNumber(key_pressed), getFrequencyOfNote(key_pressed));
      darkenDown(document.getElementById(key_pressed));
      return true;
    }
    return false;
  };

  /**
   * Determine whether pressed key is a modifier key or not.
   * @param {KeyboardEvent} The keydown event of a pressed key
   */
  var isModifierKey = function (key) {
    return key.ctrlKey || key.metaKey || key.altKey;
  };

  /**
   * Add event listeners to keyboard.
   * @param {element} keyboard_element
   */
  var addListeners = function (container) {
    var that = this; // Store reference to 'this'

    if (settings.musicalTyping) {
      addKeyboardListener.call(this);
    }

    // Mouse is clicked down on keyboard element.
    container.addEventListener('mousedown', function (event) {
      mouseDown(event.target, that.keyDown);
    });

    // Mouse is released from keyboard element.
    container.addEventListener('mouseup', function (event) {
      mouseUp(event.target, that.keyUp);
    });

    // Mouse is moved over keyboard element.
    container.addEventListener('mouseover', function (event) {
      mouseOver(event.target, that.keyDown);
    });

    // Mouse is moved out of keyboard element.
    container.addEventListener('mouseout', function (event) {
      mouseOut(event.target, that.keyUp);
    });

    // Device supports touch events.
    if ('ontouchstart' in document.documentElement) {
      container.addEventListener('touchstart', function (event) {
        mouseDown(event.target, that.keyDown);
      });

      container.addEventListener('touchend', function (event) {
        mouseUp(event.target, that.keyUp);
      });

      container.addEventListener('touchleave', function (event) {
        mouseOut(event.target, that.keyUp);
      });

      container.addEventListener('touchcancel', function (event) {
        mouseOut(event.target, that.keyUp);
      });
    }
  };

  var addKeyboardListener = function () {
    var that = this;

    globalWindow.addEventListener('keydown', function (key) {
      if (isModifierKey(key)) {
        return;
      }
      // In custom keyMap mode, use settings.keyMap (the user-provided map);
      // otherwise fall back to the built-in key_map.
      var activeMap = settings.customKeyMap ? settings.keyMap : key_map;
      if (key.keyCode in activeMap) {
        key.preventDefault();
        if (key.keyCode in keysDown) {
          return;
        }
        keysDown[key.keyCode] = true;
        var note = getKeyPressed(key.keyCode);
        if (!note) return;
        var el = document.getElementById(note);
        lightenUp(el);
        that.keyDown(getMidiNoteNumber(note), getFrequencyOfNote(note));
      }
    });

    globalWindow.addEventListener('keyup', function (key) {
      if (isModifierKey(key)) {
        return;
      }
      var activeMap = settings.customKeyMap ? settings.keyMap : key_map;
      if (key.keyCode in activeMap) {
        delete keysDown[key.keyCode];
        var note = getKeyPressed(key.keyCode);
        if (!note) return;
        var el = document.getElementById(note);
        darkenDown(el);
        that.keyUp(getMidiNoteNumber(note), getFrequencyOfNote(note));
      }
    });
  };

  /**
   * Qwerty Hancock constructor.
   * @param {object} settings Optional user settings.
   */
  var QwertyHancock = function (settings) {
    this.version = version;

    this.keyDown = function () {
      // Placeholder function.
    };

    this.keyUp = function () {
      // Placeholder function.
    };

    this.setKeyOctave = function (octave) {
      // Placeholder function.
    };

    this.getKeyOctave = function () {};
    this.keyOctaveUp = function () {};
    this.keyOctaveDown = function () {};

    this.getKeyMap = function () {};
    this.setKeyMap = function (newKeyMap) {};

    init.call(this, settings);
  };

  if (typeof exports !== 'undefined') {
    if (typeof module !== 'undefined' && module.exports) {
      exports = module.exports = QwertyHancock;
    }
    exports.QwertyHancock = QwertyHancock;
  } else {
    root.QwertyHancock = QwertyHancock;
  }
})(this);
