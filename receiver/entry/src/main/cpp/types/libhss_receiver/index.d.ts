export interface ReceiverStatus {
  state: string;
  detail: string;
  listenAddress: string;
  pairedAddress: string;
  deviceId: string;
  listening: boolean;
  connected: boolean;
  paired: boolean;
  automaticAddressPublished: boolean;
  automaticAddressConflict: boolean;
  automaticAddressFailed: boolean;
  framesDecoded: number;
  framesDropped: number;
  receivedFrames: number;
  mediaWidth: number;
  mediaHeight: number;
  mediaMaxFps: number;
}

export interface PairingRecord {
  deviceId: string;
  senderId: string;
  credential: string;
  version: number;
}

export const startReceiver: (listenAddress: string) => boolean;
export const stopReceiver: () => void;
export const configureTrust: (
  deviceId: string,
  senderId: string,
  credential: string,
  version: number
) => boolean;
export const authorizeQr: (
  sessionId: string,
  token: string,
  expiresAtMs: number
) => boolean;
export const authorizeShortCode: (shortCode: string) => boolean;
export const forgetDevice: () => void;
export const getStatus: () => ReceiverStatus;
export const getPairing: () => PairingRecord;
export const getWifiAddresses: () => string[];
export const onAppForeground: () => void;
export const onAppBackground: () => void;

declare const receiver: {
  startReceiver: typeof startReceiver;
  stopReceiver: typeof stopReceiver;
  configureTrust: typeof configureTrust;
  authorizeQr: typeof authorizeQr;
  authorizeShortCode: typeof authorizeShortCode;
  forgetDevice: typeof forgetDevice;
  getStatus: typeof getStatus;
  getPairing: typeof getPairing;
  getWifiAddresses: typeof getWifiAddresses;
  onAppForeground: typeof onAppForeground;
  onAppBackground: typeof onAppBackground;
};

export default receiver;
