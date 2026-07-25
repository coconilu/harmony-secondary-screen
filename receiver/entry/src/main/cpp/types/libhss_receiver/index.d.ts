export interface ReceiverStatus {
  state: string;
  detail: string;
  listenAddress: string;
  pairedAddress: string;
  deviceId: string;
  listening: boolean;
  connected: boolean;
  paired: boolean;
  framesDecoded: number;
  framesDropped: number;
  receivedFrames: number;
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
};

export default receiver;
