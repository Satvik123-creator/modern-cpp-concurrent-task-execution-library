import React from 'react';
import { usePolling } from '../hooks/usePolling';
import { getQueue } from '../services/api';
import LoadingSpinner from '../components/LoadingSpinner';
import ErrorAlert from '../components/ErrorAlert';

export default function QueuePage() {
  const { data, loading, error, refresh } = usePolling(() => getQueue(), 1500);

  if (loading) return <LoadingSpinner text="Loading queue..." />;
  if (error) return <ErrorAlert message={error} onRetry={refresh} />;

  const size = data?.size ?? 0;
  const capacity = data?.capacity ?? 0;
  const isFull = data?.is_full ?? false;
  const pct = capacity > 0 ? (size / capacity) * 100 : 0;

  return (
    <div className="space-y-6">
      <h2 className="text-2xl font-bold">Queue</h2>

      <div className="grid grid-cols-1 md:grid-cols-3 gap-4">
        <div className="bg-white dark:bg-gray-900 rounded-xl border border-gray-200 dark:border-gray-800 p-5">
          <p className="text-xs text-gray-500 uppercase tracking-wider">Current Size</p>
          <p className="text-4xl font-bold mt-2">{size}</p>
        </div>
        <div className="bg-white dark:bg-gray-900 rounded-xl border border-gray-200 dark:border-gray-800 p-5">
          <p className="text-xs text-gray-500 uppercase tracking-wider">Capacity</p>
          <p className="text-4xl font-bold mt-2">{capacity}</p>
        </div>
        <div className="bg-white dark:bg-gray-900 rounded-xl border border-gray-200 dark:border-gray-800 p-5">
          <p className="text-xs text-gray-500 uppercase tracking-wider">Status</p>
          <p className={`text-4xl font-bold mt-2 ${isFull ? 'text-red-500' : 'text-green-500'}`}>
            {isFull ? 'FULL' : 'OK'}
          </p>
        </div>
      </div>

      <div className="bg-white dark:bg-gray-900 rounded-xl border border-gray-200 dark:border-gray-800 p-5">
        <h3 className="font-semibold mb-4">Capacity Usage</h3>
        <div className="w-full bg-gray-200 dark:bg-gray-700 rounded-full h-6 overflow-hidden">
          <div
            className={`h-full rounded-full transition-all duration-500 ${
              pct > 90 ? 'bg-red-500' : pct > 70 ? 'bg-yellow-500' : 'bg-blue-500'
            }`}
            style={{ width: `${Math.min(pct, 100)}%` }}
          />
        </div>
        <div className="flex justify-between mt-2 text-sm text-gray-400">
          <span>{size} queued</span>
          <span>{pct.toFixed(1)}%</span>
          <span>{capacity} max</span>
        </div>
      </div>
    </div>
  );
}
