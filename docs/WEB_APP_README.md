# Facial Recognition Lock App 

This is a code bundle for Facial Recog Lock App. My original prototype is available at https://www.figma.com/design/1EHUlhKR7BT5MwUyVx4lmV/Facial-Recog-Lock-App. Available on request: [@linkedin:post](https://lnkd.in/p/e4GnH59w)

## Running the code

Run `npm i` to install the dependencies.

Run `npm run dev` to start the development server.

## Other Resources

- The firmware for the lock is available at https://github.com/Leospars/Facial_Recognition_Smart_Lock_Firmware.
- The SBC code is available at https://github.com/Leospars/jupy_lock_sbc.git

## What Was Implemented

### 1. Authentication System (Supabase-based)

- **AuthContext** (`/components/AuthContext.tsx`): Global authentication state management
- **LoginPage** (`/components/LoginPage.tsx`): Full-screen login/signup interface
- Email/password authentication
- Google OAuth integration (requires setup)
- Apple OAuth integration (requires setup)
- Session persistence and auto-login
- Logout functionality
- Redirect to Login Page if unauthenticated or expired

### 2. QR Code Scanner with Real Camera Access

- **QRScanner** (`/components/QRScanner.tsx`): Camera-based QR code scanning
- Uses browser's MediaDevices API to access camera
- Visual scanning overlay with animated scanning line
- Dev Bypass to use set pairing code and BLE ID (though not currently necessary in the interface but can be used for faster automated pairing)

### 3. Modified Add Door Flow

- **AddDoorDialog** (`/components/AddDoorDialog.tsx`): Updated to check authentication
- Integrates QRScanner for real camera access
- Shows device ID from scanned QR code
- Confirms door addition with user

### 4. Event Logging System

- **Backend Routes** (`src/supabase/functions/server/index.tsx`):
  - `POST /make-server-a213de84/signup` - User registration
  - `GET /make-server-a213de84/events` - Fetch event logs (auth required)
  - `POST /make-server-a213de84/events` - Create event log (auth required)
  - `GET /make-server-a213de84/managed-users` - Fetch managed users (auth required)
  - `PUT /make-server-a213de84/managed-users/:userId` - Update user (auth required)
- **Event Logger Utility** (`/utils/eventLogger.ts`): Helper functions for logging
- **DoorControls** (`/components/DoorControls.tsx`): Auto-logs lock/unlock events

### 5. App Integration

- **App.tsx**: Updated with AuthProvider wrapper
- Profile menu shows login status
- Light/Dark Mode Settings
- Service Worker to handle Push Notifications from the lock remotely
- Logout functionality

## OAuth Setup Requirements

**IMPORTANT**: Google and Apple login will NOT work until you complete the OAuth setup in Supabase:

1. **Google**: Follow https://supabase.com/docs/guides/auth/social-login/auth-google
2. **Apple**: Follow https://supabase.com/docs/guides/auth/social-login/auth-apple

See `/OAUTH_SETUP.md` for detailed instructions.

## Security Considerations

1. **Access Tokens**: All protected routes validate JWT tokens
2. **Service Role Key**: Never exposed to frontend (server-only)
3. **Email Confirmation**: Auto-confirmed since email server not configured
4. **CORS**: Configured for all origins (adjust for production)

## Testing the Implementation

### Test Authentication

1. Click profile avatar → "Login / Sign Up"
2. Create account with email/password
3. Should see "Account created successfully!" and auto-login
4. Profile menu should now show user's initials

### Test Add Door

1. Without login: Click "Add Door" → Should show login page
2. After login: Click "Add Door" → Should allow camera access
3. Grant camera permission when prompted
4. Scanner shows live camera feed with overlay
5. After 2 seconds, QR code is "detected"
6. Review screen shows door name and device ID

### Test Event Logging

1. Login to the system
2. Go to "Doors" tab
3. Toggle a door lock
4. Go to "Manage Users" → "Event Logs"
5. Should see new event logged (if backend is working)
6. Falls back to mock data if backend unavailable

### Test Local Camera

1. Click on a registered lock on the "Locks Dashboard Page"
2. Ensure the Single Board Computer (SBC/Raspberry Pi) is plugged in
3. Click the Gallery Icon
4. Press the appropriate button to start camera feed

## Testing

1. **Managed users**: Sharing lock privileges hasn't been tested

## Future Enhancements

1. Create fingerprint registration flow
2. Add user invitation system
3. Implement access schedules and temporary access

## Screnshots

* Lock Setup Dialog

  <img src=./images/screenshot_lock_setup_dialog.png alt="Lock Setup Dialog Image" width=690px>
* Successful Lock Setup

  <img src=./images/screenshot_successful_lock_register.png alt="Successful Lock Setup Image" width=600px >
* Lock Details

  <img src=./images/screenshot_lock_details_page.png alt="Lock Details Image" width=690px>
* Gallery Database

  <img src=./images/screenshot_gallery_database.png alt="Gallery Database Image" width=600px>
