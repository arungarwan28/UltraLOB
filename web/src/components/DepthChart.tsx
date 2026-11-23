import { useEffect, useRef } from 'react';
import * as echarts from 'echarts';
import { formatPrice } from '../lib/format';
import type { Level } from '../lib/types';

interface DepthChartProps {
  bids: Level[];
  asks: Level[];
  height?: number;
}

export function DepthChart({ bids, asks, height = 260 }: DepthChartProps) {
  const container = useRef<HTMLDivElement | null>(null);
  const chart = useRef<ReturnType<typeof echarts.init> | null>(null);

  useEffect(() => {
    if (container.current === null) return;
    const instance = echarts.init(container.current, undefined, { renderer: 'canvas' });
    chart.current = instance;
    const resize = () => instance.resize();
    window.addEventListener('resize', resize);
    return () => {
      window.removeEventListener('resize', resize);
      instance.dispose();
      chart.current = null;
    };
  }, []);

  useEffect(() => {
    const instance = chart.current;
    if (instance === null) return;

    let bidSum = 0;
    const bidPoints = [...bids]
      .reverse()
      .map((level) => {
        bidSum += level.qty;
        return [level.price / 100, bidSum] as [number, number];
      });

    let askSum = 0;
    const askPoints = asks.map((level) => {
      askSum += level.qty;
      return [level.price / 100, askSum] as [number, number];
    });

    instance.setOption(
      {
        animation: false,
        backgroundColor: 'transparent',
        grid: { left: 44, right: 16, top: 16, bottom: 28 },
        tooltip: {
          trigger: 'axis',
          backgroundColor: '#0e131c',
          borderColor: '#1c2634',
          textStyle: { color: '#b9c6d8', fontSize: 11 },
          valueFormatter: (value: unknown) => `${value} units (cumulative)`,
        },
        xAxis: {
          type: 'value',
          scale: true,
          axisLine: { lineStyle: { color: '#1c2634' } },
          axisLabel: {
            color: '#8595ad',
            fontSize: 10,
            formatter: (value: number) => value.toFixed(2),
          },
          splitLine: { lineStyle: { color: 'rgba(28,38,52,0.5)' } },
        },
        yAxis: {
          type: 'value',
          axisLabel: { color: '#8595ad', fontSize: 10 },
          splitLine: { lineStyle: { color: 'rgba(28,38,52,0.5)' } },
        },
        series: [
          {
            name: 'bids',
            type: 'line',
            step: 'end',
            data: bidPoints,
            showSymbol: false,
            lineStyle: { color: '#4ade80', width: 1.5 },
            areaStyle: { color: 'rgba(74,222,128,0.16)' },
          },
          {
            name: 'asks',
            type: 'line',
            step: 'start',
            data: askPoints,
            showSymbol: false,
            lineStyle: { color: '#f2697c', width: 1.5 },
            areaStyle: { color: 'rgba(242,105,124,0.16)' },
          },
        ],
      },
      { notMerge: true },
    );
  }, [bids, asks]);

  return (
    <div className="relative">
      <div ref={container} style={{ height }} className="w-full" />
      {bids.length === 0 && asks.length === 0 && (
        <div className="pointer-events-none absolute inset-0 flex items-center justify-center text-[11px] text-fog/60">
          book is empty — submit resting orders to build depth
        </div>
      )}
      <div className="mt-1 flex items-center justify-center gap-4 text-[10px] uppercase tracking-[0.12em] text-fog/70">
        <span className="text-bid">bids {bids.length > 0 ? formatPrice(bids[0].price) : '—'}</span>
        <span className="text-ask">asks {asks.length > 0 ? formatPrice(asks[0].price) : '—'}</span>
      </div>
    </div>
  );
}
