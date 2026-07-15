import React from 'react';
import { usePolling } from '../hooks/usePolling';
import { getWorkers } from '../services/api';
import StatusBadge from '../components/StatusBadge';
import LoadingSpinner from '../components/LoadingSpinner';
import ErrorAlert from '../components/ErrorAlert';

const STATE_ORDER = ['running', 'waiting', 'idle', 'stopped'];

export default function WorkersPage() {
  const { data, loading, error, refresh } = usePolling(() => getWorkers(), 2000);

  if (loading) return <LoadingSpinner text="Loading workers..." />;
  if (error) return <ErrorAlert message={error} onRetry={refresh} />;

  const workers = data?.workers ?? [];
  const stats = data?.pool ?? {};

  const running = workers.filter((w) => w.state === 'running').length;
  const waiting = workers.filter((w) => w.state === 'waiting').length;
  const stopped = workers.filter((w) => w.state === 'stopped').length;

  return (
    <div className="space-y-6">
      <div className="flex items-center justify-between">
        <h2 className="text-2xl font-bold">Workers</h2>
        <span className="text-sm text-gray-400">{workers.length} total</span>
      </div>

      <div className="grid grid-cols-2 md:grid-cols-4 gap-4">
        <div className="bg-white dark:bg-gray-900 rounded-xl border border-gray-200 dark:border-gray-800 p-4">
          <p className="text-xs text-gray-500 uppercase tracking-wider">Total</p>
          <p className="text-3xl font-bold mt-1">{workers.length}</p>
        </div>
        <div className="bg-white dark:bg-gray-900 rounded-xl border border-gray-200 dark:border-gray-800 p-4">
          <p className="text-xs text-gray-500 uppercase tracking-wider">Running</p>
          <p className="text-3xl font-bold mt-1 text-blue-500">{running}</p>
        </div>
        <div className="bg-white dark:bg-gray-900 rounded-xl border border-gray-200 dark:border-gray-800 p-4">
          <p className="text-xs text-gray-500 uppercase tracking-wider">Idle</p>
          <p className="text-3xl font-bold mt-1 text-green-500">{waiting}</p>
        </div>
        <div className="bg-white dark:bg-gray-900 rounded-xl border border-gray-200 dark:border-gray-800 p-4">
          <p className="text-xs text-gray-500 uppercase tracking-wider">Stopped</p>
          <p className="text-3xl font-bold mt-1 text-red-500">{stopped}</p>
        </div>
      </div>

      <div className="bg-white dark:bg-gray-900 rounded-xl border border-gray-200 dark:border-gray-800">
        <div className="px-5 py-4 border-b border-gray-200 dark:border-gray-800">
          <h3 className="font-semibold">Worker Details</h3>
        </div>
        <div className="divide-y divide-gray-200 dark:divide-gray-800">
          {workers.length === 0 ? (
            <p className="p-5 text-gray-400 text-center">No workers</p>
          ) : (
            workers.map((w) => (
              <div key={w.id} className="px-5 py-4 flex items-center justify-between">
                <div className="flex items-center gap-4">
                  <div className={`w-10 h-10 rounded-lg flex items-center justify-center font-bold text-sm ${
                    w.state === 'running' ? 'bg-blue-100 dark:bg-blue-900/30 text-blue-600' :
                    w.state === 'waiting' ? 'bg-green-100 dark:bg-green-900/30 text-green-600' :
                    'bg-gray-100 dark:bg-gray-800 text-gray-500'
                  }`}>
                    W{w.id}
                  </div>
                  <div>
                    <p className="font-medium">Worker {w.id}</p>
                    <p className="text-xs text-gray-400">
                      {w.state === 'running' ? 'Executing task' : w.state === 'waiting' ? 'Ready for work' : w.state}
                    </p>
                  </div>
                </div>
                <div className="flex items-center gap-3">
                  {w.marked_stop && (
                    <span className="text-xs text-orange-500 bg-orange-100 dark:bg-orange-900/30 px-2 py-0.5 rounded">Scaling down</span>
                  )}
                  <StatusBadge status={w.state} />
                </div>
              </div>
            ))
          )}
        </div>
      </div>
    </div>
  );
}
