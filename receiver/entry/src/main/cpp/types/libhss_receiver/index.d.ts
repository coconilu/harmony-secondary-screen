export interface ReceiverStatus {
  state: string;
  detail: string;
  connected: boolean;
  framesDecoded: number;
  framesDropped: number;
}

export const connect: (host: string, pairingCode: string) => boolean;
export const disconnect: () => void;
export const getStatus: () => ReceiverStatus;
export const setInputMode: (mode: 'pointer' | 'scroll') => void;

declare const receiver: {
  connect: typeof connect;
  disconnect: typeof disconnect;
  getStatus: typeof getStatus;
  setInputMode: typeof setInputMode;
};

export default receiver;
