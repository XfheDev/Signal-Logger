import { useState, useEffect, useRef, useCallback, useMemo } from 'react';
import {
  MapContainer, TileLayer, Marker, Popup, Tooltip,
  Polyline, Circle, useMap, useMapEvents
} from 'react-leaflet';
import L from 'leaflet';
import 'leaflet/dist/leaflet.css';
import './index.css';

// ═══════════════════════════════════════════════════════════════
// CONSTANTS & UTILS
// ═══════════════════════════════════════════════════════════════
const RECEIVER_DEFAULT = { lat: 41.015137, lon: 28.979530 };
const SIGNAL_TIMEOUT   = 90; // seconds — must match backend

const MIL_RANGES = [
  [0xAE0000,0xAEFFFF],[0x43C000,0x43CFFF],[0x3A4000,0x3A4FFF],
  [0x686000,0x6863FF],[0x350000,0x37FFFF],[0x140000,0x157FFF],[0x710000,0x71FFFF],
];
function isMilitary(icao) {
  try { const h=parseInt(icao,16); return MIL_RANGES.some(([l,hi])=>h>=l&&h<=hi); }
  catch { return false; }
}
function haversineKm(la1,lo1,la2,lo2) {
  const R=6371,r=x=>x*Math.PI/180,dL=r(la2-la1),dO=r(lo2-lo1);
  return R*2*Math.atan2(Math.sqrt(Math.sin(dL/2)**2+Math.cos(r(la1))*Math.cos(r(la2))*Math.sin(dO/2)**2),
                        Math.sqrt(1-(Math.sin(dL/2)**2+Math.cos(r(la1))*Math.cos(r(la2))*Math.sin(dO/2)**2)));
}
function bearingDeg(la1,lo1,la2,lo2) {
  const r=x=>x*Math.PI/180,dO=r(lo2-lo1);
  return((Math.atan2(Math.sin(dO)*Math.cos(r(la2)),Math.cos(r(la1))*Math.sin(r(la2))-Math.sin(r(la1))*Math.cos(r(la2))*Math.cos(dO))*180/Math.PI)+360)%360;
}
function projectPos(lat,lon,hdg,spd,sec=120) {
  if(!spd||!hdg||!lat||!lon) return null;
  const d=(spd*0.514444*sec)/6371000,h=hdg*Math.PI/180,la=lat*Math.PI/180,lo=lon*Math.PI/180;
  const la2=Math.asin(Math.sin(la)*Math.cos(d)+Math.cos(la)*Math.sin(d)*Math.cos(h));
  return [la2*180/Math.PI,(lo+Math.atan2(Math.sin(h)*Math.sin(d)*Math.cos(la),Math.cos(d)-Math.sin(la)*Math.sin(la2)))*180/Math.PI];
}

// Age → CSS class
function ageClass(s) {
  if (s < 0)  return 'age-unk';
  if (s < 15) return 'age-fresh';
  if (s < 45) return 'age-ok';
  if (s < 75) return 'age-warn';
  return 'age-stale';
}
function fmtAge(s) {
  if (s < 0) return '?';
  if (s < 60) return s + 's';
  return Math.floor(s/60) + 'm' + (s%60) + 's';
}

// ═══════════════════════════════════════════════════════════════
// ICONS
// ═══════════════════════════════════════════════════════════════
function acIcon(heading=0,selected=false,mil=false,age=0) {
  const stale = age > 60;
  const c = mil ? '#ffaa00' : selected ? '#ffe066' : stale ? '#6688aa' : '#00e5ff';
  const glow = mil  ? `drop-shadow(0 0 9px #ffaa00bb)`
             : selected ? `drop-shadow(0 0 8px #ffe066aa)`
             : stale ? 'none'
             : `drop-shadow(0 0 5px #00e5ff44)`;
  const op = stale ? '0.55' : '1';
  return new L.DivIcon({
    html: `<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 32 32" width="32" height="32"
      style="transform:rotate(${heading}deg);filter:${glow};opacity:${op}">
      <polygon points="16,1 20,27 16,22 12,27" fill="${c}" stroke="#000" stroke-width="1.3"/>
      <polygon points="7,15 25,15 23,21 9,21" fill="${c}" stroke="#000" stroke-width="0.8" opacity="0.75"/>
      <polygon points="12,24 20,24 19,29 13,29" fill="${c}" stroke="#000" stroke-width="0.7" opacity="0.5"/>
    </svg>`,
    className:'', iconSize:[32,32], iconAnchor:[16,16],
  });
}
function shipIcon(selected=false) {
  const c = selected ? '#ffe066' : '#ff3d6e';
  return new L.DivIcon({
    html:`<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 26 26" width="26" height="26"
      style="filter:drop-shadow(0 0 5px ${c}66)">
      <path d="M13 2L7 9h12L13 2z M4 11h18v2l-2 9H6L4 13V11z" fill="${c}" stroke="#000" stroke-width="1"/>
    </svg>`,
    className:'', iconSize:[26,26], iconAnchor:[13,13],
  });
}
function rxIcon() {
  return new L.DivIcon({
    html:`<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 36 36" width="36" height="36"
      style="filter:drop-shadow(0 0 8px #00e5ffaa)">
      <circle cx="18" cy="18" r="7" fill="none" stroke="#00e5ff" stroke-width="2"/>
      <circle cx="18" cy="18" r="2.5" fill="#00e5ff"/>
      <line x1="18" y1="1" x2="18" y2="11" stroke="#00e5ff" stroke-width="1.8" stroke-linecap="round"/>
      <line x1="18" y1="25" x2="18" y2="35" stroke="#00e5ff" stroke-width="1.8" stroke-linecap="round"/>
      <line x1="1" y1="18" x2="11" y2="18" stroke="#00e5ff" stroke-width="1.8" stroke-linecap="round"/>
      <line x1="25" y1="18" x2="35" y2="18" stroke="#00e5ff" stroke-width="1.8" stroke-linecap="round"/>
    </svg>`,
    className:'', iconSize:[36,36], iconAnchor:[18,18],
  });
}

// ═══════════════════════════════════════════════════════════════
// MAP COMPONENTS
// ═══════════════════════════════════════════════════════════════
function MapFocus({ target }) {
  const map = useMap();
  useEffect(() => {
    if (target?.lat && target?.lon && target.lat!==0)
      map.flyTo([target.lat, target.lon], 9, {animate:true, duration:1.2});
  }, [target, map]);
  return null;
}
function MapClickHandler({ onMapClick, active }) {
  useMapEvents({ click(e) { if (active) onMapClick(e.latlng); } });
  return null;
}
function MouseCoords() {
  const [pos, setPos] = useState(null);
  useMapEvents({ mousemove(e) { setPos(e.latlng); }, mouseout() { setPos(null); } });
  if (!pos) return null;
  return (
    <div className="mouse-coords">
      {pos.lat.toFixed(5)}°  {pos.lng.toFixed(5)}°
    </div>
  );
}

// ═══════════════════════════════════════════════════════════════
// STATS PANEL
// ═══════════════════════════════════════════════════════════════
function StatsPanel({ flights, receiver, onClose }) {
  const withPos   = flights.filter(f=>f.lat&&f.lon&&f.lat!==0);
  const adsb      = flights.filter(f=>f.protocol==='ADS-B');
  const ais       = flights.filter(f=>f.protocol==='AIS');
  const mil       = flights.filter(f=>isMilitary(f.icao));
  const avgAlt    = adsb.length ? Math.round(adsb.reduce((s,f)=>s+(f.altitude||0),0)/adsb.length) : 0;
  const avgSpd    = adsb.length ? Math.round(adsb.reduce((s,f)=>s+(f.speed||0),0)/adsb.length) : 0;
  const maxRange  = withPos.length ? Math.max(...withPos.map(f=>haversineKm(receiver.lat,receiver.lon,f.lat,f.lon))) : 0;

  // Altitude histogram buckets
  const buckets = [
    {l:'GND',    lo:0,     hi:1000,  count:0},
    {l:'<10k',   lo:1000,  hi:10000, count:0},
    {l:'10-25k', lo:10000, hi:25000, count:0},
    {l:'25-35k', lo:25000, hi:35000, count:0},
    {l:'35k+',   lo:35000, hi:99999, count:0},
  ];
  adsb.forEach(f=>{ const b=buckets.find(b=>f.altitude>=b.lo&&f.altitude<b.hi); if(b) b.count++; });
  const maxCount = Math.max(1, ...buckets.map(b=>b.count));

  return (
    <div className="stats-panel">
      <div className="sp-head">
        <span>📊 STATISTICS</span>
        <button onClick={onClose}>✕</button>
      </div>
      <div className="sp-grid">
        <Sc label="Total Contacts" value={flights.length} />
        <Sc label="ADS-B" value={adsb.length} />
        <Sc label="AIS" value={ais.length} />
        <Sc label="Military" value={mil.length} color={mil.length>0?'#ffaa00':null} />
        <Sc label="Avg Altitude" value={avgAlt ? avgAlt.toLocaleString()+' ft' : '—'} />
        <Sc label="Avg Speed" value={avgSpd ? avgSpd+' kt' : '—'} />
        <Sc label="Max Range" value={maxRange ? maxRange.toFixed(0)+' km' : '—'} />
        <Sc label="With Position" value={withPos.length} />
      </div>
      <div className="sp-hist-title">Altitude Distribution</div>
      <div className="sp-hist">
        {buckets.map(b=>(
          <div key={b.l} className="sp-bar-wrap">
            <div className="sp-bar-bg">
              <div className="sp-bar-fill" style={{height: `${(b.count/maxCount)*100}%`}}/>
            </div>
            <div className="sp-bar-lbl">{b.l}</div>
            <div className="sp-bar-n">{b.count}</div>
          </div>
        ))}
      </div>
    </div>
  );
}
function Sc({label, value, color}) {
  return (
    <div className="sc-item">
      <span className="sc-lbl">{label}</span>
      <span className="sc-val" style={color?{color}:{}}>{value}</span>
    </div>
  );
}

// ═══════════════════════════════════════════════════════════════
// MAIN APP
// ═══════════════════════════════════════════════════════════════
export default function App() {
  const [flights,     setFlights]    = useState([]);
  const [intercepts,  setIntercepts] = useState([]);
  const [trails,      setTrails]     = useState({});
  const [status,      setStatus]     = useState(null);
  const [selected,    setSelected]   = useState(null);
  const [filterMode,  setFilterMode] = useState('ALL');
  const [altFilter,   setAltFilter]  = useState('ALL');
  const [search,      setSearch]     = useState('');
  const [sortKey,     setSortKey]    = useState('icao');
  const [connected,   setConnected]  = useState(false);
  const [lastUpdate,  setLastUpdate] = useState(null);
  const [mapLayer,    setMapLayer]   = useState('SAT');
  const [showRings,   setShowRings]  = useState(true);
  const [showVectors, setShowVectors]= useState(true);
  const [showTrails,  setShowTrails] = useState(true);
  const [showLog,     setShowLog]    = useState(true);
  const [showStats,   setShowStats]  = useState(false);
  const [settingRx,   setSettingRx]  = useState(false);
  const [newIcaos,    setNewIcaos]   = useState(new Set()); // flash new contacts
  const [receiver,    setReceiver]   = useState(() => {
    try { return JSON.parse(localStorage.getItem('sl_rx')) || RECEIVER_DEFAULT; }
    catch { return RECEIVER_DEFAULT; }
  });
  const logRef      = useRef(null);
  const prevIcaos   = useRef(new Set());

  // ── Keyboard shortcuts ─────────────────────────────────────
  useEffect(() => {
    const h = e => {
      if (e.key==='Escape')  { setSelected(null); setSettingRx(false); }
      if (e.key==='s')       setShowStats(v=>!v);
      if (e.key==='r')       setShowRings(v=>!v);
      if (e.key==='t')       setShowTrails(v=>!v);
      if (e.key==='v')       setShowVectors(v=>!v);
      if (e.key==='l')       setShowLog(v=>!v);
    };
    window.addEventListener('keydown', h);
    return () => window.removeEventListener('keydown', h);
  }, []);

  // ── Data fetch ─────────────────────────────────────────────
  const fetchFlights = useCallback(async () => {
    try {
      const [fRes, sRes] = await Promise.all([
        fetch('http://localhost:8000/api/flights'),
        fetch('http://localhost:8000/api/status'),
      ]);
      const fd = await fRes.json(); const sd = await sRes.json();
      setConnected(true); setLastUpdate(new Date()); setStatus(sd);

      if (fd.status === 'ok') {
        const arr = fd.flights || [];
        const currIcaos = new Set(arr.map(f=>f.icao));

        // Detect new contacts for flash animation
        const brandNew = [...currIcaos].filter(ic => !prevIcaos.current.has(ic));
        if (brandNew.length > 0) {
          setNewIcaos(prev => {
            const n = new Set([...prev, ...brandNew]);
            // Clear after 3 seconds
            setTimeout(() => setNewIcaos(p => { const x=new Set(p); brandNew.forEach(ic=>x.delete(ic)); return x; }), 3000);
            return n;
          });
        }
        prevIcaos.current = currIcaos;

        setFlights(arr);

        // Update trails — only for active contacts; prune disappeared ones
        setTrails(prev => {
          const t = {};
          arr.forEach(f => {
            if (f.lat && f.lon && f.lat!==0 && f.lon!==0) {
              const existing = prev[f.icao] || [];
              const last = existing.at(-1);
              if (!last || last[0]!==f.lat || last[1]!==f.lon)
                t[f.icao] = [...existing, [f.lat,f.lon]].slice(-80);
              else
                t[f.icao] = existing;
            }
          });
          // Contacts not in arr → trail drops too (signal lost = trail gone)
          return t;
        });

        // Keep selected in sync; clear if contact lost signal
        setSelected(prev => {
          if (!prev) return null;
          const found = arr.find(f=>f.icao===prev.icao);
          return found || null; // null = signal lost → deselect
        });
      }
    } catch { setConnected(false); }
  }, []);

  const fetchIntercepts = useCallback(async () => {
    try {
      const r = await fetch('http://localhost:8000/api/intercepts?limit=120');
      const d = await r.json();
      if (d.intercepts) setIntercepts(d.intercepts);
    } catch {}
  }, []);

  useEffect(() => {
    fetchFlights(); fetchIntercepts();
    const i1 = setInterval(fetchFlights,   1000);
    const i2 = setInterval(fetchIntercepts, 2000);
    return () => { clearInterval(i1); clearInterval(i2); };
  }, [fetchFlights, fetchIntercepts]);

  useEffect(() => {
    if (logRef.current) logRef.current.scrollTop = logRef.current.scrollHeight;
  }, [intercepts]);

  // ── Filter + Sort ──────────────────────────────────────────
  const filtered = useMemo(() => {
    return flights
      .filter(f => {
        if (filterMode!=='ALL' && f.protocol!==filterMode) return false;
        const a = f.altitude||0;
        if (altFilter==='GND'    && a>1000)               return false;
        if (altFilter==='LOW'    && !(a>1000&&a<10000))   return false;
        if (altFilter==='MED'    && !(a>=10000&&a<25000)) return false;
        if (altFilter==='HIGH'   && !(a>=25000&&a<35000)) return false;
        if (altFilter==='CRUISE' && a<35000)              return false;
        if (search) {
          const q=search.toLowerCase();
          return f.icao?.toLowerCase().includes(q)||f.callsign?.toLowerCase().includes(q);
        }
        return true;
      })
      .sort((a,b) => {
        if (sortKey==='alt')   return (b.altitude||0)-(a.altitude||0);
        if (sortKey==='speed') return (b.speed||0)-(a.speed||0);
        if (sortKey==='age')   return (a.age_seconds||0)-(b.age_seconds||0);
        if (sortKey==='dist')
          return haversineKm(receiver.lat,receiver.lon,a.lat||0,a.lon||0)
               - haversineKm(receiver.lat,receiver.lon,b.lat||0,b.lon||0);
        if (sortKey==='mil')   return (isMilitary(b.icao)?1:0)-(isMilitary(a.icao)?1:0);
        return (a.icao||'').localeCompare(b.icao||'');
      });
  }, [flights, filterMode, altFilter, search, sortKey, receiver]);

  const mapFlights = useMemo(()=>filtered.filter(f=>f.lat&&f.lon&&f.lat!==0),[filtered]);
  const milCount   = useMemo(()=>flights.filter(f=>isMilitary(f.icao)).length,[flights]);

  // ── Receiver ───────────────────────────────────────────────
  const handleMapClick = useCallback(latlng => {
    const r = {lat:latlng.lat, lon:latlng.lng};
    setReceiver(r);
    localStorage.setItem('sl_rx', JSON.stringify(r));
    setSettingRx(false);
  }, []);

  // ── Export ─────────────────────────────────────────────────
  const exportJSON = () => {
    const blob = new Blob([JSON.stringify(flights,null,2)],{type:'application/json'});
    const url = URL.createObjectURL(blob);
    const a = document.createElement('a');
    a.href=url; a.download=`signal-logger-${new Date().toISOString().slice(0,19)}.json`;
    a.click(); URL.revokeObjectURL(url);
  };

  // ── Selected helpers ───────────────────────────────────────
  const sel     = selected;
  const selMil  = sel ? isMilitary(sel.icao) : false;
  const selDist = sel?.lat ? haversineKm(receiver.lat,receiver.lon,sel.lat,sel.lon).toFixed(1) : null;
  const selBear = sel?.lat ? bearingDeg(receiver.lat,receiver.lon,sel.lat,sel.lon).toFixed(0) : null;

  // ── Tiles ──────────────────────────────────────────────────
  const tiles = mapLayer==='SAT'
    ? { url:'https://server.arcgisonline.com/ArcGIS/rest/services/World_Imagery/MapServer/tile/{z}/{y}/{x}', attr:'© Esri' }
    : { url:'https://{s}.basemaps.cartocdn.com/dark_all/{z}/{x}/{y}{r}.png', attr:'© CARTO', sub:'abcd' };

  const RINGS = [50,100,200,300].map(nm=>nm*1852);

  // ──────────────────────────────────────────────────────────
  return (
    <div className={`app${settingRx?' cursor-cross':''}`}>

      {/* ══ TOPBAR ══ */}
      <header className="topbar">
        <div className="topbar-brand">
          <div className="brand-icon-wrap">
            <span className="brand-pulse-ring"/>
            <span className="brand-pulse-dot"/>
          </div>
          <div>
            <div className="brand-name">SIGNAL LOGGER</div>
            <div className="brand-sub">Advanced SIGINT Platform</div>
          </div>
        </div>

        <div className="tb-group">
          <TBtn active={mapLayer==='SAT'}  onClick={()=>setMapLayer('SAT')}>🛰 SAT</TBtn>
          <TBtn active={mapLayer==='DARK'} onClick={()=>setMapLayer('DARK')}>🌙 DARK</TBtn>
          <TBtn active={showRings}   onClick={()=>setShowRings(v=>!v)}>⊙ RINGS</TBtn>
          <TBtn active={showVectors} onClick={()=>setShowVectors(v=>!v)}>⇒ VECTOR</TBtn>
          <TBtn active={showTrails}  onClick={()=>setShowTrails(v=>!v)}>〰 TRAIL</TBtn>
        </div>

        <div className="tb-group">
          {['ALL','ADS-B','AIS'].map(m=>(
            <TBtn key={m} active={filterMode===m} onClick={()=>setFilterMode(m)}>
              {m==='ALL'?'🌍 All':m==='ADS-B'?'✈ ADS-B':'🚢 AIS'}
            </TBtn>
          ))}
        </div>

        <div className="tb-stats">
          <span className={`live-pill ${connected?'ok':'err'}`}>
            <span className="live-dot"/>{connected?'LIVE':'OFFLINE'}
          </span>
          <span className="stat-chip">✈ {flights.filter(f=>f.protocol==='ADS-B').length}</span>
          <span className="stat-chip">🚢 {flights.filter(f=>f.protocol==='AIS').length}</span>
          {milCount>0 && <span className="stat-chip mil-chip">⚠ MIL {milCount}</span>}
          {status && <span className="stat-chip">{status.packets_total} pkts</span>}
          {status && <span className="stat-chip">⏱ {status.uptime}</span>}
          {lastUpdate && <span className="stat-chip dim">{lastUpdate.toLocaleTimeString()}</span>}
          <TBtn active={showStats} onClick={()=>setShowStats(v=>!v)}>📊 Stats</TBtn>
          <TBtn onClick={exportJSON}>⬇ Export</TBtn>
          <TBtn active={settingRx} danger={settingRx} onClick={()=>setSettingRx(v=>!v)}>
            📡 {settingRx?'Click Map…':'Antenna'}
          </TBtn>
        </div>
      </header>

      {/* Keyboard hint strip */}
      <div className="kb-hint">
        ESC=deselect · S=stats · R=rings · T=trails · V=vectors · L=log
      </div>

      <div className="main-layout">

        {/* ══ SIDEBAR ══ */}
        <aside className="sidebar">
          <input className="search-box" placeholder="🔍 ICAO / Callsign..."
            value={search} onChange={e=>setSearch(e.target.value)}/>

          <div className="ctrl-row">
            <span className="ctrl-lbl">Sort</span>
            {[['icao','ICAO'],['alt','ALT'],['speed','SPD'],['dist','DIST'],['age','AGE'],['mil','MIL']].map(([k,l])=>(
              <button key={k} className={`mini-btn${sortKey===k?' on':''}`} onClick={()=>setSortKey(k)}>{l}</button>
            ))}
          </div>

          <div className="ctrl-row">
            <span className="ctrl-lbl">Alt</span>
            {[['ALL','ALL'],['GND','GND'],['LOW','<10k'],['MED','10-25k'],['HIGH','25-35k'],['CRUISE','35k+']].map(([k,l])=>(
              <button key={k} className={`mini-btn${altFilter===k?' on':''}`} onClick={()=>setAltFilter(k)}>{l}</button>
            ))}
          </div>

          <div className="contact-count">
            {filtered.length} contacts
            {newIcaos.size > 0 && <span className="new-badge"> +{newIcaos.size} NEW</span>}
          </div>

          <div className="ftable-wrap">
            <table className="ftable">
              <thead><tr>
                <th/><th>ICAO</th><th>Callsign</th>
                <th>Alt</th><th>Spd</th><th>Hdg</th><th>V/S</th><th>Age</th>
              </tr></thead>
              <tbody>
                {filtered.map(f => {
                  const mil  = isMilitary(f.icao);
                  const isSel= sel?.icao===f.icao;
                  const isNew= newIcaos.has(f.icao);
                  const age  = f.age_seconds ?? -1;
                  return (
                    <tr key={f.icao}
                      className={[isSel?'sel':'', isNew?'row-new':'', ageClass(age)].join(' ')}
                      onClick={()=>setSelected(f)}>
                      <td>{mil?<span className="m-badge">MIL</span>:f.protocol==='AIS'?'🚢':'✈'}</td>
                      <td className={`icao-cell${mil?' mil':''}`}>{f.icao}</td>
                      <td className="cs-cell">{f.callsign!==f.icao?f.callsign:'—'}</td>
                      <td>{f.altitude?f.altitude.toLocaleString():'—'}</td>
                      <td>{f.speed?Math.round(f.speed):'—'}</td>
                      <td>{f.heading?Math.round(f.heading)+'°':'—'}</td>
                      <td>
                        {(f.vert_rate||0)>100?<span className="vup">↑</span>
                          :(f.vert_rate||0)<-100?<span className="vdn">↓</span>
                          :<span className="vlv">—</span>}
                      </td>
                      <td className={`age-cell ${ageClass(age)}`}>{fmtAge(age)}</td>
                    </tr>
                  );
                })}
                {filtered.length===0 && (
                  <tr><td colSpan={8} className="empty-row">Sinyal bekleniyor...</td></tr>
                )}
              </tbody>
            </table>
          </div>
        </aside>

        {/* ══ MAP ══ */}
        <div className="map-wrap">
          <MapContainer center={[41.0,28.97]} zoom={6}
            style={{width:'100%',height:'100%'}} zoomControl={true}>

            <TileLayer url={tiles.url} attribution={tiles.attr} subdomains={tiles.sub||'abc'} maxZoom={19}/>
            {mapLayer==='SAT' && (
              <TileLayer
                url="https://server.arcgisonline.com/ArcGIS/rest/services/Reference/World_Boundaries_and_Places/MapServer/tile/{z}/{y}/{x}"
                attribution="" opacity={0.65}/>
            )}

            <MapFocus target={sel}/>
            <MapClickHandler onMapClick={handleMapClick} active={settingRx}/>
            <MouseCoords/>

            {/* Receiver */}
            <Marker position={[receiver.lat,receiver.lon]} icon={rxIcon()}>
              <Tooltip permanent direction="right" offset={[16,0]} className="rx-tt">
                📡 RX {receiver.lat.toFixed(3)}, {receiver.lon.toFixed(3)}
              </Tooltip>
            </Marker>

            {/* Range rings */}
            {showRings && RINGS.map((r,i)=>(
              <Circle key={r} center={[receiver.lat,receiver.lon]} radius={r}
                pathOptions={{color:'#00e5ff',weight:1,opacity:0.15+i*0.05,fillOpacity:0,dashArray:'5 8'}}/>
            ))}

            {/* Trails */}
            {showTrails && Object.entries(trails).map(([icao,path])=>{
              if (path.length<2) return null;
              const f = flights.find(x=>x.icao===icao);
              if (!f||(filterMode!=='ALL'&&f.protocol!==filterMode)) return null;
              const mil = isMilitary(icao);
              return (
                <Polyline key={`tr-${icao}`} positions={path}
                  pathOptions={{color:mil?'#ffaa00':f.protocol==='AIS'?'#ff3d6e':'#00e5ff',
                    weight:1.5, opacity:0.4, dashArray:'3 5'}}/>
              );
            })}

            {/* Velocity vectors */}
            {showVectors && mapFlights.map(f=>{
              const proj = projectPos(f.lat,f.lon,f.heading,f.speed,120);
              if (!proj) return null;
              return (
                <Polyline key={`vv-${f.icao}`} positions={[[f.lat,f.lon],proj]}
                  pathOptions={{color:'#ffffff',weight:1,opacity:0.25,dashArray:'2 5'}}/>
              );
            })}

            {/* Markers */}
            {mapFlights.map(f=>{
              const isShip=f.protocol==='AIS', isSel=sel?.icao===f.icao, mil=isMilitary(f.icao);
              const age=f.age_seconds??0;
              const icon = isShip ? shipIcon(isSel) : acIcon(f.heading||0,isSel,mil,age);
              return (
                <Marker key={f.icao} position={[f.lat,f.lon]} icon={icon}
                  zIndexOffset={isSel?1000:mil?500:0}
                  eventHandlers={{click:()=>setSelected(f)}}>
                  <Tooltip direction="right" offset={[14,0]} opacity={1} permanent className="ac-tt">
                    <span className={mil?'tt-mil':'tt-norm'}>
                      {f.callsign!==f.icao?f.callsign:f.icao}
                    </span><br/>
                    <span className="tt-sub">
                      {f.altitude?f.altitude.toLocaleString()+'ft':''}
                      {f.speed?' · '+Math.round(f.speed)+'kt':''}
                      {age>0?' · '+fmtAge(age):''}
                    </span>
                  </Tooltip>
                  <Popup className="ac-popup">
                    <div className="pu-head">{f.icao} {f.callsign!==f.icao&&<span>({f.callsign})</span>}</div>
                    <PuRow l="Protocol" v={f.protocol}/>
                    <PuRow l="Altitude" v={`${(f.altitude||0).toLocaleString()} ft`}/>
                    <PuRow l="Speed"    v={`${Math.round(f.speed||0)} kt`}/>
                    <PuRow l="Heading"  v={`${Math.round(f.heading||0)}°`}/>
                    <PuRow l="V/S"      v={`${f.vert_rate||0} ft/min`}/>
                    <PuRow l="Age"      v={fmtAge(age)}/>
                    <PuRow l="Position" v={`${f.lat?.toFixed(4)}, ${f.lon?.toFixed(4)}`}/>
                  </Popup>
                </Marker>
              );
            })}
          </MapContainer>

          {!connected && (
            <div className="map-offline">
              ⚠️ API bağlantısı yok<br/>
              <code>python dashboard/backend/main.py</code>
            </div>
          )}
          {settingRx && (
            <div className="map-hint">📡 Anten konumunu seçmek için haritaya tıklayın · ESC iptal</div>
          )}
          {showRings && (
            <div className="ring-legend">
              {[50,100,200,300].map(nm=><span key={nm} className="ring-lbl">{nm} NM</span>)}
            </div>
          )}

          {/* Stats panel (floating) */}
          {showStats && (
            <StatsPanel flights={flights} receiver={receiver} onClose={()=>setShowStats(false)}/>
          )}
        </div>

        {/* ══ DETAIL PANEL ══ */}
        {sel && (
          <aside className="detail-panel">
            <div className="dp-head">
              <div className="dp-title">
                <span className={`dp-icao${selMil?' mil':''}`}>{sel.icao}</span>
                {selMil && <span className="dp-mil">MIL</span>}
                {sel.protocol==='AIS' && <span className="dp-ais">AIS</span>}
              </div>
              {sel.callsign!==sel.icao && <div className="dp-cs">{sel.callsign}</div>}
              <button className="dp-close" onClick={()=>setSelected(null)}>✕</button>
            </div>

            {/* Signal age bar */}
            {sel.age_seconds >= 0 && (
              <div className="age-bar-wrap">
                <div className="age-bar-track">
                  <div className="age-bar-fill"
                    style={{width:`${Math.max(2,100-((sel.age_seconds/SIGNAL_TIMEOUT)*100))}%`,
                      background: sel.age_seconds<15?'#00ff88':sel.age_seconds<45?'#00e5ff':sel.age_seconds<70?'#ffaa00':'#ff3d6e'}}/>
                </div>
                <span className="age-bar-lbl">Signal: {fmtAge(sel.age_seconds)} ago</span>
              </div>
            )}

            {sel.heading>0 && (
              <div className="compass-wrap">
                <svg viewBox="0 0 90 90" width="90" height="90">
                  <circle cx="45" cy="45" r="40" fill="none" stroke="#1a2840" strokeWidth="1.5"/>
                  <circle cx="45" cy="45" r="32" fill="none" stroke="#1a2840" strokeWidth="0.5" strokeDasharray="2 4"/>
                  {['N','E','S','W'].map((d,i)=>{
                    const a=i*90*Math.PI/180;
                    return <text key={d} x={45+38*Math.sin(a)} y={45-38*Math.cos(a)+4}
                      textAnchor="middle" fill="#4a6070" fontSize="8">{d}</text>;
                  })}
                  <line x1="45" y1="45"
                    x2={45+32*Math.sin(sel.heading*Math.PI/180)}
                    y2={45-32*Math.cos(sel.heading*Math.PI/180)}
                    stroke="#00e5ff" strokeWidth="2.5" strokeLinecap="round"/>
                  <circle cx="45" cy="45" r="3" fill="#00e5ff"/>
                  <text x="45" y="88" textAnchor="middle" fill="#4a6070" fontSize="7">
                    {Math.round(sel.heading)}° · {Math.round(sel.speed||0)} kt
                  </text>
                </svg>
              </div>
            )}

            <Dsec title="TELEMETRY">
              <DR l="Altitude"   v={sel.altitude?sel.altitude.toLocaleString()+' ft':'—'}/>
              <DR l="Speed"      v={sel.speed?Math.round(sel.speed)+' kt':'—'}/>
              <DR l="Heading"    v={sel.heading?Math.round(sel.heading)+'°':'—'}/>
              <DR l="Vert Rate"  v={sel.vert_rate
                ?<span className={sel.vert_rate>0?'vup':'vdn'}>{sel.vert_rate>0?'↑ +':'↓ '}{sel.vert_rate} ft/min</span>:'—'}/>
              <DR l="Frequency"  v={sel.frequency?sel.frequency+' MHz':'—'}/>
              <DR l="Signal Age" v={<span className={ageClass(sel.age_seconds??-1)}>{fmtAge(sel.age_seconds??-1)}</span>}/>
            </Dsec>

            <Dsec title="POSITION">
              <DR l="Latitude"   v={sel.lat?sel.lat.toFixed(6)+'°':'—'}/>
              <DR l="Longitude"  v={sel.lon?sel.lon.toFixed(6)+'°':'—'}/>
              <DR l="Range (RX)" v={selDist?selDist+' km':'—'}/>
              <DR l="Bear (RX)"  v={selBear?selBear+'°':'—'}/>
            </Dsec>

            <Dsec title="INTELLIGENCE">
              <DR l="Category" v={selMil?<span className="vup">⚠ Military</span>:sel.protocol==='AIS'?'🚢 Marine':'✈ Civil'}/>
              <DR l="Protocol" v={sel.protocol}/>
              <DR l="ICAO Hex" v={<code>{sel.icao}</code>}/>
              <DR l="Last Seen" v={sel.timestamp?sel.timestamp.slice(11,19):'—'}/>
            </Dsec>
          </aside>
        )}
      </div>

      {/* ══ INTERCEPT LOG ══ */}
      <div className="ic-bar">
        <div className="ic-head" onClick={()=>setShowLog(v=>!v)}>
          <span className="ic-title">📡 INTERCEPT LOG</span>
          <span className="ic-count">{intercepts.length} frames · {SIGNAL_TIMEOUT}s timeout</span>
          <span className="ic-toggle">{showLog?'▾':'▴'}</span>
        </div>
        {showLog && (
          <div className="ic-body" ref={logRef}>
            {intercepts.slice().reverse().map((row,i)=>{
              const mil=isMilitary(row.Identifier||'');
              return (
                <div key={i} className={`ic-row${mil?' ic-mil':row.Protocol==='AIS'?' ic-ais':''}`}>
                  <span className="ic-ts">{(row.Timestamp||'').slice(11,23)}</span>
                  <span className={`ic-proto ${row.Protocol==='AIS'?'ais':'adsb'}`}>{row.Protocol}</span>
                  <span className={`ic-id${mil?' mil':''}`}>{row.Identifier}</span>
                  {row.Callsign&&<span className="ic-f cs">{row.Callsign}</span>}
                  {row.Altitude&&row.Altitude!=='0'&&<span className="ic-f">ALT:<b>{row.Altitude}</b></span>}
                  {row.Speed&&row.Speed!=='0.0'&&<span className="ic-f">SPD:<b>{Math.round(row.Speed)}</b></span>}
                  {row.Heading&&row.Heading!=='0.0'&&<span className="ic-f">HDG:<b>{Math.round(row.Heading)}°</b></span>}
                  {row.VertRate&&row.VertRate!=='0'&&<span className="ic-f">VRT:<b>{row.VertRate}</b></span>}
                  {row.Latitude&&row.Latitude!=='0.000000'&&<span className="ic-f">LAT:<b>{parseFloat(row.Latitude).toFixed(3)}</b></span>}
                  {row.Longitude&&row.Longitude!=='0.000000'&&<span className="ic-f">LON:<b>{parseFloat(row.Longitude).toFixed(3)}</b></span>}
                  <span className="ic-freq">{row.Frequency} MHz</span>
                </div>
              );
            })}
            {intercepts.length===0&&<div className="ic-empty">C++ engine çalışıyor mu? Sinyal bekleniyor...</div>}
          </div>
        )}
      </div>
    </div>
  );
}

// ── Helpers ──────────────────────────────────────────────────
function TBtn({children,active,danger,onClick}) {
  return <button className={`tbtn${active?' on':''}${danger?' danger':''}`} onClick={onClick}>{children}</button>;
}
function Dsec({title,children}) {
  return <div className="dp-section"><div className="dp-sec-title">{title}</div>{children}</div>;
}
function DR({l,v}) {
  return <div className="dp-row"><span className="dp-lbl">{l}</span><span className="dp-val">{v}</span></div>;
}
function PuRow({l,v}) {
  return <div className="pu-row"><span>{l}</span><b>{v}</b></div>;
}
