export interface ReceiverStatus {
  state: string;
  detail: string;
  listenAddress: string;
  pairingCode: string;
  pairedAddress: string;
  listening: boolean;
  connected: boolean;
  framesDecoded: number;
  framesDropped: number;
}

export const startReceiver: (listenAddress: string) => boolean;
export const stopReceiver: () => void;
export const getStatus: () => ReceiverStatus;

declare const receiver: {
  startReceiver: typeof startReceiver;
  stopReceiver: typeof stopReceiver;
  getStatus: typeof getStatus;
};

export default receiver;
