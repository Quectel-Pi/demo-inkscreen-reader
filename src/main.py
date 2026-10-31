#!/home/pi/mediapipe_env/bin/python3
import os
import cv2
import time
import numpy as np
from evdev import UInput, ecodes
import mediapipe as mp

# ======================
# Parameter section (critical for project)
# ======================
FRAME_WIDTH = 640
FRAME_HEIGHT = 480

# Iris position change threshold (relative change)
IRIS_CHANGE_THRESHOLD = 0.008
# Threshold for significant iris position change

# Page turn cooldown time
COOLDOWN_TIME = 1.0

# Smoothing parameter
SMOOTHING_FACTOR = 0.3  # Exponential smoothing coefficient (0-1, smaller means smoother)

# Initialization waiting time (to prevent false triggering during startup)
INITIALIZATION_PERIOD = 4.0  # Wait 4 seconds after startup before responding to page turns

# Screen off related parameters
SCREEN_OFF_TIMEOUT = 60.0  # How long to send screen off signal after no face detected (seconds)
screen_off_sent_time = 0  # Time when screen off signal was sent
screen_is_off = False  # Whether the screen is off

# State variables
last_action_time = 0
read_to_bottom = False  # Flag to mark if already read to the bottom of the screen
smooth_iris_y = None    # Smoothed iris Y position
reference_iris_y = None # Reference iris Y position (when stationary)
eye_movement_buffer = []  # Eye movement direction buffer
BUFFER_SIZE = 5          # Buffer size

# Add a variable to track when face was just detected
face_just_detected_time = 0  # Time when face was just detected
FACE_DETECTION_COOLDOWN = 2.0  # Cooldown time after face detection

# Wake-up safety delay parameters
WAKE_UP_SAFETY_DELAY = 3.0  # Do not respond to eye movement page turns for 3 seconds after wake-up
wake_up_time = 0  # Record wake-up time

# Initialization related variables
initialization_start_time = time.time()
is_initialized = False  # Whether initialization is completed

# ======================
# OPTIONAL: eye-control preview UI (HTTP/MJPEG), enable with INK_PREVIEW=1
# ======================
# PREVIEW_UI_PATCHED
preview = None
if os.environ.get("INK_PREVIEW", "1") != "0":
    try:
        import preview_server
        _web = os.environ.get("INK_PREVIEW_WEB", "0") not in ("0", "off", "false", "")
        preview = preview_server.start(
            int(os.environ.get("INK_PREVIEW_PORT", "8088")), web=_web)
        if _web:
            print(f"[INFO] Eye control preview: web page on port {preview.port}", flush=True)
        else:
            print("[INFO] Eye control preview: native window only "
                  "(set INK_PREVIEW_WEB=1 for the web page)", flush=True)
        _gui = os.environ.get("INK_PREVIEW_GUI", "window").lower()
        _size = os.environ.get("INK_PREVIEW_GUI_SIZE", "720x480").lower()
        try:
            _sw, _sh = [int(v) for v in _size.replace("*", "x").split("x")[:2]]
        except Exception:
            _sw, _sh = 720, 480
        if _gui not in ("0", "off", "none", "false") and os.environ.get("DISPLAY"):
            preview.enable_gui(mode=_gui, size=(_sw, _sh),
                               fit=os.environ.get("INK_PREVIEW_GUI_FIT", "cover"),
                               pos=os.environ.get("INK_PREVIEW_GUI_POS", ""))
        else:
            print("[INFO] Native window skipped (no DISPLAY or INK_PREVIEW_GUI=0)", flush=True)
    except Exception as _pv_err:
        print(f"[WARN] Preview UI disabled: {_pv_err}", flush=True)
        preview = None


# ======================
# Camera auto-detection function
# ======================
def find_available_camera():
    """Automatically detect available camera devices"""
    print("[INFO] Searching for available cameras...")

    # Try camera indices 0-9
    for i in range(10):
        device_path = f"/dev/video{i}"
        if os.path.exists(device_path):
            print(f"[INFO] Device path exists: {device_path}")
            temp_cap = cv2.VideoCapture(i)
            if temp_cap.isOpened():
                ret, frame = temp_cap.read()
                if ret:
                    print(f"[INFO] Found available camera at index: {i}")
                    temp_cap.release()
                    return i
                else:
                    print(f"[WARNING] Camera {i} opened but failed to read frame")
                    temp_cap.release()
            else:
                print(f"[WARNING] Failed to open camera at index: {i}")
        else:
            print(f"[INFO] Device path does not exist: {device_path}")
    
    raise RuntimeError("No camera found")

# ======================
# Virtual key initialization
# ======================
# Create a standard keyboard device to simulate key events
ui = UInput({
    ecodes.EV_KEY: [
        ecodes.KEY_PAGEDOWN,  # Next page
        ecodes.KEY_PAGEUP,   # Previous page
        ecodes.BTN_LEFT,     # Screen off signal (using mouse left button as replacement)
        ecodes.BTN_RIGHT     # Wake-up signal (using mouse right button as replacement)
    ]
}, name="eye_page_turner", version=0x3)

def send_key(key):
    ui.write(ecodes.EV_KEY, key, 1)
    ui.write(ecodes.EV_KEY, key, 0)
    ui.syn()
    if preview:
        preview.flash(("PAGE DOWN" if key == ecodes.KEY_PAGEDOWN else str(key)))

# ======================
# MediaPipe initialization
# ======================
mp_face = mp.solutions.face_mesh
face_mesh = mp_face.FaceMesh(
    static_image_mode=False,
    max_num_faces=1,
    refine_landmarks=True,
    min_detection_confidence=0.5,
    min_tracking_confidence=0.5
)

# Iris landmark indices
LEFT_IRIS = [474, 475, 476, 477]
RIGHT_IRIS = [469, 470, 471, 472]

# Eye contour landmarks (for calculating eye center)
LEFT_EYE = [33, 133, 160, 159, 158, 144, 145, 153]
RIGHT_EYE = [362, 263, 387, 386, 385, 373, 374, 380]

# ======================
# Camera initialization
# ======================
CAMERA_INDEX = None
while CAMERA_INDEX is None:
    try:
        CAMERA_INDEX = find_available_camera()
    except RuntimeError:
        print("[WARNING] No camera detected, retrying in 5s "
              "(preview stays available)", flush=True)
        if preview:
            preview.flash("NO CAMERA")
        time.sleep(5)
print(f"[INFO] Using camera index {CAMERA_INDEX}", flush=True)
cap = cv2.VideoCapture(CAMERA_INDEX)
cap.set(cv2.CAP_PROP_FRAME_WIDTH, FRAME_WIDTH)
cap.set(cv2.CAP_PROP_FRAME_HEIGHT, FRAME_HEIGHT)

if not cap.isOpened():
    raise RuntimeError("Camera open failed")

print("[INFO] Eye control started")

def eye_center(landmarks, idxs):
    """Calculate eye center point"""
    x_sum = sum(landmarks[i].x for i in idxs)
    y_sum = sum(landmarks[i].y for i in idxs)
    count = len(idxs)
    return x_sum/count, y_sum/count

def iris_center(landmarks, idxs):
    """Calculate iris center point"""
    x_sum = sum(landmarks[i].x for i in idxs)
    y_sum = sum(landmarks[i].y for i in idxs)
    count = len(idxs)
    return x_sum/count, y_sum/count

# ======================
# Main loop
# ======================
while True:
    ret, frame = cap.read()
    if not ret:
        break

    frame = cv2.flip(frame, 1)
    rgb = cv2.cvtColor(frame, cv2.COLOR_BGR2RGB)
    result = face_mesh.process(rgb)

    now = time.time()

    if preview:
        preview.update(frame, result)
        if preview.gui_enabled:
            preview.show()
        preview.set_state(initialized=is_initialized,
                          init_left=max(0.0, INITIALIZATION_PERIOD - (now - initialization_start_time)),
                          threshold=IRIS_CHANGE_THRESHOLD,
                          cooldown=COOLDOWN_TIME,
                          last_action=last_action_time,
                          read_to_bottom=read_to_bottom)

    # Check if initialization is completed
    if not is_initialized:
        if now - initialization_start_time >= INITIALIZATION_PERIOD:
            is_initialized = True
            print(f"[INFO] Initialization completed after {INITIALIZATION_PERIOD} seconds")
        else:
            # Skip the following display steps
            continue

    # Detect if face is present
    face_detected = result.multi_face_landmarks is not None and len(result.multi_face_landmarks) > 0
    
    if not face_detected:
        if preview:
            preview.set_state(face=False, screen_off=screen_is_off,
                              relative_change=None, buffer=[])
        # If face was previously detected but now not detected, start screen off timer
        if not screen_off_sent_time:
            screen_off_sent_time = now
        
        # Check if exceeded screen off time threshold
        if now - screen_off_sent_time >= SCREEN_OFF_TIMEOUT and not screen_is_off:
            screen_is_off = True
            print("[INFO] Sending screen OFF signal due to no face detected")
            send_key(ecodes.BTN_LEFT)  # Send screen off signal, using BTN_LEFT instead of KEY_SLEEP
    else:
        # Reset screen off timer when face is re-detected
        screen_off_sent_time = 0
        
        # If screen was previously off, now need to send wake-up signal
        if screen_is_off:
            screen_is_off = False
            print("[INFO] Sending screen ON signal - Face detected, attempting wake up")
            send_key(ecodes.BTN_RIGHT)  # Send wake-up signal, using BTN_RIGHT instead of KEY_WAKEUP
            
            # Clear all accumulated input events during screen-off period to prevent accidental page turns
            try:
                # Attempt to read and discard all pending events
                import select
                while select.select([ui.fd], [], [], 0) == ([ui.fd], [], []):
                    ui.read()
                print("[INFO] Cleared accumulated events during screen-off period")
            except:
                # If unable to clear events, ignore error and continue execution
                pass
                
            time.sleep(0.5)  # Brief delay to ensure wake-up signal has been processed
            
            # Record wake-up time for subsequent page down command and safety delay
            wake_up_time = time.time()
            # Record time when face was just detected, to start new cooldown period
            face_just_detected_time = now
            print(f"[INFO] Wake up detected, safety delay of {WAKE_UP_SAFETY_DELAY}s activated")
            print("[INFO] Will send page down in 2 seconds after wake up")
            
        # Only process eye movement control logic when screen is not off
        if not screen_is_off:
            # Check if within cooldown period after face detection, if yes, do not process eye movement control
            if now - face_just_detected_time <= FACE_DETECTION_COOLDOWN:
                # During cooldown after face detection, do not execute any page turn operations
                continue
            else:
                # Check if need to send page down command after wake-up
                if wake_up_time != 0 and time.time() - wake_up_time >= 2.0:
                    print("[ACTION] SENDING PAGE DOWN AFTER WAKE UP DELAY")
                    send_key(ecodes.KEY_PAGEDOWN)
                    last_action_time = time.time()  # Update last action time
                    wake_up_time = 0  # Reset wake-up time marker
                    # Skip subsequent eye movement control logic to avoid duplicate page turns
                    continue
                
                if result.multi_face_landmarks and len(result.multi_face_landmarks) > 0:
                    lm = result.multi_face_landmarks[0].landmark
                else:
                    continue  
            

            # Get eye center points
            left_eye_x, left_eye_y = eye_center(lm, LEFT_EYE)
            right_eye_x, right_eye_y = eye_center(lm, RIGHT_EYE)
            
            # Get iris center points
            left_iris_x, left_iris_y = iris_center(lm, LEFT_IRIS)
            right_iris_x, right_iris_y = iris_center(lm, RIGHT_IRIS)
            
            # Calculate average Y coordinate of both irises (normalized value)
            avg_iris_y = (left_iris_y + right_iris_y) / 2.0
            
            # Apply exponential smoothing
            if smooth_iris_y is None:
                smooth_iris_y = avg_iris_y
            else:
                smooth_iris_y = smooth_iris_y * (1 - SMOOTHING_FACTOR) + avg_iris_y * SMOOTHING_FACTOR
            
            # Initialize reference position (when head is stable)
            if reference_iris_y is None:
                reference_iris_y = smooth_iris_y
                print(f"[INFO] Initial reference iris Y: {reference_iris_y:.4f}")
            
            # Calculate relative change (compared to reference position)
            relative_change = smooth_iris_y - reference_iris_y

            if preview:
                preview.set_state(face=True, screen_off=screen_is_off,
                                  smooth_iris_y=smooth_iris_y,
                                  reference_iris_y=reference_iris_y,
                                  relative_change=relative_change,
                                  buffer=list(eye_movement_buffer),
                                  read_to_bottom=read_to_bottom)
            
            # Add movement direction to buffer
            if len(eye_movement_buffer) >= BUFFER_SIZE:
                eye_movement_buffer.pop(0)
            
            # Determine movement direction for current frame
            if relative_change > IRIS_CHANGE_THRESHOLD:
                eye_movement_buffer.append("DOWN")
            elif relative_change < -IRIS_CHANGE_THRESHOLD:
                eye_movement_buffer.append("UP")
            else:
                eye_movement_buffer.append("NEUTRAL")
            
            # Analyze movement pattern in buffer
            down_count = eye_movement_buffer.count("DOWN")
            up_count = eye_movement_buffer.count("UP")
            
            # Detect downward gaze pattern (consecutive frames looking down)
            if down_count >= BUFFER_SIZE * 0.7:  # 70% of frames are looking down
                read_to_bottom = True
                reference_iris_y = smooth_iris_y  # Update reference position
            
            # Detect upward gaze pattern (consecutive frames looking up)
            elif up_count >= BUFFER_SIZE * 0.7 and read_to_bottom:
                # Check if within wake-up safety delay period
                if wake_up_time != 0 and now - wake_up_time < WAKE_UP_SAFETY_DELAY:
                    # During safety delay period, do not respond to eye movement page turns
                    pass
                elif now - last_action_time >= COOLDOWN_TIME:
                    print("[ACTION] NEXT PAGE (Look Up)")
                    send_key(ecodes.KEY_PAGEDOWN)
                    last_action_time = now
                    read_to_bottom = False  # Reset state
                    reference_iris_y = smooth_iris_y  # Update reference position
                    eye_movement_buffer.clear()  # Clear buffer
                else:
                    pass
            
            # If eyes return to neutral position and not read to bottom, update reference position
            elif up_count >= BUFFER_SIZE * 0.7 and not read_to_bottom:
                reference_iris_y = smooth_iris_y

cap.release()
ui.close()
# cv2.destroyAllWindows()