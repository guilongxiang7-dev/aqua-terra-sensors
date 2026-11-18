/* Aqua / Terra — conceptual interactive diorama. This is an illustration,
 * not a reconstruction of the project's instruments or live measurements.
 * All graphics are drawn locally. No external libraries or network requests.
 */
(() => {
  'use strict';
  const canvas = document.getElementById('world-canvas');
  if (!canvas) return;
  const ctx = canvas.getContext('2d', { alpha: true });
  if (!ctx) return;
  const motionPreference = window.matchMedia('(prefers-reduced-motion: reduce)');
  let reducedMotion = motionPreference.matches;
  let w = 620, h = 540, dpr = 1, scale = 120;
  let mode = 'both', paused = reducedMotion, visible = true;
  let yaw = -0.38, targetYaw = yaw, pitch = 0.55, targetPitch = pitch;
  let time = 0, previousTime = 0, raf = 0;
  let dragging = false, pointerId = null, startX = 0, startY = 0;
  let startYaw = 0, startPitch = 0;
  let shapes = [], annotations = [];
  const clamp = (v, a, b) => Math.min(b, Math.max(a, v));
  const mix = (a, b, t) => a + (b - a) * t;
  const rgba = (r, g, b, a = 1) => `rgba(${r | 0},${g | 0},${b | 0},${a})`;
  const hash = (a, b = 0) => {
    const x = Math.sin(a * 127.1 + b * 311.7) * 43758.5453123;
    return x - Math.floor(x);
  };

  function project(p) {
    const c = Math.cos(yaw), s = Math.sin(yaw);
    const x = p[0] * c - p[2] * s;
    const z = p[0] * s + p[2] * c;
    return {
      x: w * 0.5 + x * scale,
      y: h * 0.59 + (z * Math.sin(pitch) - p[1] * Math.cos(pitch)) * scale,
      depth: z * Math.cos(pitch) + p[1] * Math.sin(pitch)
    };
  }
  function polygon(points, fill, stroke, width = 1, extra = {}) {
    const projected = points.map(project);
    shapes.push({ points: projected, fill, stroke, width,
      depth: projected.reduce((n, p) => n + p.depth, 0) / projected.length,
      ...extra });
  }
  function line(points, color, width = 1, extra = {}) {
    polygon(points, null, color, width, { open: true, ...extra });
  }
  function box(cx, cy, cz, sx, sy, sz, colors, stroke) {
    const x0 = cx - sx / 2, x1 = cx + sx / 2;
    const y0 = cy - sy / 2, y1 = cy + sy / 2;
    const z0 = cz - sz / 2, z1 = cz + sz / 2;
    const vs = [
      [[x0,y0,z0],[x1,y0,z0],[x1,y1,z0],[x0,y1,z0]],
      [[x1,y0,z0],[x1,y0,z1],[x1,y1,z1],[x1,y1,z0]],
      [[x1,y0,z1],[x0,y0,z1],[x0,y1,z1],[x1,y1,z1]],
      [[x0,y0,z1],[x0,y0,z0],[x0,y1,z0],[x0,y1,z1]],
      [[x0,y1,z0],[x1,y1,z0],[x1,y1,z1],[x0,y1,z1]]
    ];
    vs.forEach((v, i) => polygon(v, colors[i] || colors[0], stroke, 0.6));
  }
  function cylinder(cx, cz, bottom, top, radius, color, capColor, n = 14) {
    const cap = [];
    for (let i = 0; i < n; i++) {
      const a = i / n * Math.PI * 2, b = (i + 1) / n * Math.PI * 2;
      const p = [cx + Math.cos(a) * radius, top, cz + Math.sin(a) * radius];
      const q = [cx + Math.cos(b) * radius, top, cz + Math.sin(b) * radius];
      const light = 0.48 + 0.52 * ((Math.cos(a - 0.8) + 1) / 2);
      polygon([[p[0],bottom,p[2]],[q[0],bottom,q[2]],q,p],
        rgba(color[0] * light, color[1] * light, color[2] * light), null);
      cap.push(p);
    }
    polygon(cap, capColor || rgba(...color), 'rgba(230,250,241,.36)', 0.5);
  }
  function waterHeight(x, z) {
    return 0.37 + Math.sin(x * 6.7 + time * 1.5) * .023 + Math.cos(z * 7.5 - time * 1.1) * .022;
  }
  function terrainHeight(x, z) {
    return .35 + Math.sin(x * 4.3 + .7) * .07 + Math.cos(z * 4.7) * .053
      + Math.sin(x * 9 + z * 6) * .023 + Math.cos(x * 15 - z * 12) * .012;
  }
  function water(cx, cz, hover) {
    const half = .76, bottom = -.69 + hover, surface = .37 + hover;
    const corner = [[-half,-half],[half,-half],[half,half],[-half,half]];
    for (let k = 0; k < 4; k++) {
      const a = corner[k], b = corner[(k + 1) % 4];
      for (let j = 0; j < 9; j++) {
        const lo = mix(bottom, surface, j / 9), hi = mix(bottom, surface, (j + 1) / 9);
        const lum = j / 9;
        polygon([[cx+a[0],lo,cz+a[1]],[cx+b[0],lo,cz+b[1]],
          [cx+b[0],hi,cz+b[1]],[cx+a[0],hi,cz+a[1]]],
          rgba(43+lum*28,137+lum*54,141+lum*39,.37), null);
      }
      line([[cx+a[0],bottom,cz+a[1]],[cx+a[0],surface,cz+a[1]]], 'rgba(161,244,217,.47)', 0.8);
      line([[cx+a[0],bottom,cz+a[1]],[cx+b[0],bottom,cz+b[1]]], 'rgba(146,225,206,.4)', 0.7);
    }
    const n = 20;
    for (let ix = 0; ix < n; ix++) for (let iz = 0; iz < n; iz++) {
      const x = -half + ix / n * half * 2, z = -half + iz / n * half * 2;
      const x1 = x + half * 2 / n, z1 = z + half * 2 / n;
      const y = waterHeight(x, z);
      const wave = Math.sin(x * 6.7 + time * 1.5) * .5 + Math.cos(z * 7.5 - time * 1.1) * .5;
      polygon([[cx+x,y+hover,cz+z],[cx+x1,waterHeight(x1,z)+hover,cz+z],
        [cx+x1,waterHeight(x1,z1)+hover,cz+z1],[cx+x,waterHeight(x,z1)+hover,cz+z1]],
        rgba(79+wave*11,193+wave*13,185+wave*11,.82), 'rgba(195,255,233,.1)', .4);
    }
    for (let i = 0; i < 11; i++) {
      const v = -half + i / 10 * half * 2, path = [];
      for (let j = 0; j <= 25; j++) {
        const x = -half + j / 25 * half * 2;
        path.push([cx+x,waterHeight(x,v)+hover+.004,cz+v]);
      }
      line(path, 'rgba(207,255,234,.3)', .65);
    }
    for (let i = 0; i < 3; i++) {
      const progress = ((time * .15 + i / 3) % 1);
      const radius = .12 + progress * .38, points = [];
      for (let j = 0; j <= 60; j++) {
        const a = j / 60 * Math.PI * 2, x = Math.cos(a) * radius - .13, z = Math.sin(a) * radius - .05;
        points.push([cx+x,waterHeight(x,z)+hover+.015,cz+z]);
      }
      line(points, rgba(232,255,240,.62*(1-progress)), 1.1);
    }
    cylinder(cx-.13,cz-.05,.02+hover,1.49+hover,.058,[199,223,212],'#deeee1');
    cylinder(cx-.13,cz-.05,1.39+hover,1.7+hover,.094,[39,65,59],'#63837a');
    cylinder(cx-.13,cz-.05,.01+hover,.18+hover,.064,[87,222,189],'#b9ffe4');
    cylinder(cx-.13,cz-.05,1.45+hover,1.48+hover,.098,[113,225,191],'#b6e8d0');
    const cable=[];
    for(let i=0;i<=30;i++){
      const t=i/30;
      cable.push([cx-.13-.52*t,1.7+hover+.31*Math.sin(t*Math.PI),cz-.05-.29*t]);
    }
    line(cable,'rgba(5,25,24,.95)',5.2);
    line(cable,'rgba(159,196,177,.56)',1.05);
    annotations.push({point:[cx+.57,surface+.06,cz+.6],color:'#baffdf',r:2.2});
  }
  function soil(cx, cz, hover) {
    const half = .76, bottom = -.75 + hover;
    const edges = [[[-half,-half],[half,-half]],[[half,-half],[half,half]],[[half,half],[-half,half]],[[-half,half],[-half,-half]]];
    const colors=[[78,61,45],[103,72,49],[130,88,56],[156,107,66],[173,127,82],[144,114,72]];
    edges.forEach((edge, k)=>{
      const [a,b]=edge;
      for(let layer=0;layer<6;layer++) for(let step=0;step<18;step++){
        const s0=step/18,s1=(step+1)/18;
        const x0=mix(a[0],b[0],s0),z0=mix(a[1],b[1],s0);
        const x1=mix(a[0],b[0],s1),z1=mix(a[1],b[1],s1);
        const texture=Math.sin(step*.8+k)*.023;
        const l0=layer/6,l1=(layer+1)/6;
        const y00=mix(bottom,terrainHeight(x0,z0)+hover,l0)+texture*Math.sin(l0*Math.PI);
        const y01=mix(bottom,terrainHeight(x1,z1)+hover,l0)+texture*Math.sin(l0*Math.PI);
        const y10=mix(bottom,terrainHeight(x0,z0)+hover,l1)+texture*Math.sin(l1*Math.PI);
        const y11=mix(bottom,terrainHeight(x1,z1)+hover,l1)+texture*Math.sin(l1*Math.PI);
        const c=colors[layer],shade=.88+hash(step+layer*29,k)*.22;
        polygon([[cx+x0,y00,cz+z0],[cx+x1,y01,cz+z1],[cx+x1,y11,cz+z1],[cx+x0,y10,cz+z0]],
          rgba(c[0]*shade,c[1]*shade,c[2]*shade),null);
        if(layer===1||layer===3)line([[cx+x0,y10+.005,cz+z0],[cx+x1,y11+.005,cz+z1]],'rgba(235,197,130,.19)',.55);
      }
      for(let i=0;i<34;i++){
        const t=hash(i+61,k),y=bottom+hash(i+190,k)*.91;
        const x=mix(a[0],b[0],t),z=mix(a[1],b[1],t);
        const size=.008+hash(i,k)*.016;
        line([[cx+x,y,cz+z],[cx+x+(b[0]-a[0])*size,y+.007,cz+z+(b[1]-a[1])*size]],'rgba(243,217,157,.30)',1);
      }
    });
    const n=20;
    for(let ix=0;ix<n;ix++)for(let iz=0;iz<n;iz++){
      const x=-half+ix/n*half*2,z=-half+iz/n*half*2,x1=x+half*2/n,z1=z+half*2/n;
      const height=terrainHeight(x,z),noise=hash(ix,iz),lit=(height-.35)*1.8;
      polygon([[cx+x,height+hover,cz+z],[cx+x1,terrainHeight(x1,z)+hover,cz+z],
        [cx+x1,terrainHeight(x1,z1)+hover,cz+z1],[cx+x,terrainHeight(x,z1)+hover,cz+z1]],
        rgba(150+noise*22+lit*47,136+noise*18+lit*35,89+noise*16+lit*26), 'rgba(218,220,160,.10)', .4);
    }
    for(let iz=0;iz<=12;iz++){
      const z=-half+iz/12*half*2,path=[];
      for(let j=0;j<=40;j++){
        const x=-half+j/40*half*2;path.push([cx+x,terrainHeight(x,z)+hover+.004,cz+z]);
      }
      line(path,'rgba(232,230,171,.28)',.65);
    }
    for(let i=0;i<22;i++){
      const x=(hash(i,2)-.5)*1.35,z=(hash(i,9)-.5)*1.35;
      const y=terrainHeight(x,z)+hover+.01;
      line([[cx+x,y,cz+z],[cx+x+.018,y+.025,cz+z+.009]],'rgba(204,208,129,.55)',1.4);
    }
    for(let i=-1;i<=1;i++)cylinder(cx+i*.13,cz-.06,.05+hover,.94+hover,.025,[207,218,197],'#d9e4d1',10);
    box(cx,1.13+hover,cz-.06,.5,.48,.24,['#273c34','#2c473b','#365447','#203c32','#728471'],'rgba(193,219,183,.45)');
    box(cx,1.15+hover,cz+.064,.30,.17,.008,['#101f1a','#15281f','#15281f','#15281f','#15281f']);
    line([[cx-.105,1.15+hover,cz+.07],[cx+.105,1.15+hover,cz+.07]],'#b2cda3',1);
    line([[cx-.105,1.105+hover,cz+.07],[cx+.01,1.105+hover,cz+.07]],'#779976',.8);
    cylinder(cx,cz-.06,1.37+hover,1.45+hover,.043,[37,58,45],'#788b70',10);
    const cable=[];
    for(let i=0;i<=28;i++){
      const t=i/28; cable.push([cx+.57*t,1.45+hover+.2*Math.sin(t*Math.PI),cz-.06-.4*t]);
    }
    line(cable,'rgba(12,26,20,.95)',4.7);
    line(cable,'rgba(179,196,151,.4)',.9);
    annotations.push({point:[cx+.52,terrainHeight(.52,.57)+hover+.04,cz+.57],color:'#e6cc96',r:2.2});
  }
  function drawShape(shape) {
    if(!shape.points.length)return;
    ctx.beginPath();ctx.moveTo(shape.points[0].x,shape.points[0].y);
    for(let i=1;i<shape.points.length;i++)ctx.lineTo(shape.points[i].x,shape.points[i].y);
    if(!shape.open)ctx.closePath();
    if(shape.fill){ctx.fillStyle=shape.fill;ctx.fill();}
    if(shape.stroke){ctx.strokeStyle=shape.stroke;ctx.lineWidth=shape.width;ctx.lineJoin='round';ctx.lineCap='round';ctx.stroke();}
  }
  function backdrop() {
    const glow=ctx.createRadialGradient(w*.43,h*.46,1,w*.43,h*.46,w*.6);
    glow.addColorStop(0,'rgba(92,157,128,.11)');glow.addColorStop(1,'rgba(10,36,34,0)');
    ctx.fillStyle=glow;ctx.fillRect(0,0,w,h);
    for(let i=-5;i<=5;i++){
      const a=project([i*.45,-1.0,-2.15]),b=project([i*.45,-1.0,2.15]);
      const c=project([-2.15,-1.0,i*.45]),d=project([2.15,-1.0,i*.45]);
      ctx.strokeStyle='rgba(152,191,170,.055)';ctx.lineWidth=.7;
      ctx.beginPath();ctx.moveTo(a.x,a.y);ctx.lineTo(b.x,b.y);ctx.moveTo(c.x,c.y);ctx.lineTo(d.x,d.y);ctx.stroke();
    }
    const shadow=project([0,-1.05,.1]);
    ctx.save();ctx.translate(shadow.x,shadow.y);ctx.scale(1,.25);
    const gradient=ctx.createRadialGradient(0,0,scale*.18,0,0,scale*2.3);
    gradient.addColorStop(0,'rgba(0,12,11,.52)');gradient.addColorStop(1,'rgba(0,12,11,0)');
    ctx.fillStyle=gradient;ctx.beginPath();ctx.arc(0,0,scale*2.3,0,Math.PI*2);ctx.fill();ctx.restore();
    for(let i=0;i<23;i++){
      const x=w*(.08+hash(i,7)*.84),y=h*(.1+hash(i,18)*.72);
      const alpha=.08+.11*Math.sin(i+time*.3)**2;
      ctx.fillStyle=rgba(182,222,198,alpha);ctx.beginPath();ctx.arc(x,y,.7,0,Math.PI*2);ctx.fill();
    }
  }
  function render() {
    ctx.clearRect(0,0,w,h);shapes=[];annotations=[];backdrop();
    const drift=(!paused&&!reducedMotion)?Math.sin(time*.5)*.025:0;
    if(mode==='both'){
      water(-.88,-.18,.09+drift);soil(.91,.18,-.07-drift);
    }else if(mode==='water')water(0,0,.015+drift);
    else soil(0,0,.015+drift);
    shapes.sort((a,b)=>a.depth-b.depth);
    for(const shape of shapes)drawShape(shape);
    for(const annotation of annotations){
      const p=project(annotation.point);
      ctx.save();ctx.shadowColor=annotation.color;ctx.shadowBlur=9;ctx.fillStyle=annotation.color;
      ctx.beginPath();ctx.arc(p.x,p.y,annotation.r,0,Math.PI*2);ctx.fill();ctx.restore();
      ctx.strokeStyle=annotation.color+'44';ctx.lineWidth=.7;ctx.beginPath();ctx.arc(p.x,p.y,6,0,Math.PI*2);ctx.stroke();
    }
  }
  function frame(stamp) {
    raf=0;
    const dt=previousTime?Math.min((stamp-previousTime)/1000,.05):0;previousTime=stamp;
    if(!paused&&!reducedMotion)time+=dt;
    const target=targetYaw+(!paused&&!dragging&&!reducedMotion?Math.sin(time*.16)*.045:0);
    yaw=mix(yaw,target,.14);pitch=mix(pitch,targetPitch,.14);render();
    if(visible&&!document.hidden&&(!paused||dragging||Math.abs(yaw-targetYaw)>.0005||Math.abs(pitch-targetPitch)>.0005))raf=requestAnimationFrame(frame);
  }
  function requestFrame(){if(!raf&&visible&&!document.hidden){previousTime=0;raf=requestAnimationFrame(frame);}}
  function resize(){
    const rect=canvas.getBoundingClientRect();w=Math.max(1,rect.width);h=Math.max(1,rect.height);
    dpr=Math.min(window.devicePixelRatio||1,2);canvas.width=Math.round(w*dpr);canvas.height=Math.round(h*dpr);
    ctx.setTransform(dpr,0,0,dpr,0,0);scale=Math.min(w/(mode==='both'?4.1:2.8),h/3.7);requestFrame();
  }
  canvas.style.touchAction='pan-y';canvas.style.cursor='grab';
  canvas.addEventListener('pointerdown',event=>{
    if(event.pointerType==='mouse'&&event.button!==0)return;
    dragging=true;pointerId=event.pointerId;startX=event.clientX;startY=event.clientY;
    startYaw=targetYaw;startPitch=targetPitch;canvas.setPointerCapture(event.pointerId);canvas.style.cursor='grabbing';requestFrame();
  });
  canvas.addEventListener('pointermove',event=>{
    if(!dragging||event.pointerId!==pointerId)return;
    targetYaw=startYaw+(event.clientX-startX)*.006;
    if(event.pointerType==='mouse')targetPitch=clamp(startPitch-(event.clientY-startY)*.003,.28,.85);
    requestFrame();
  });
  const endDrag=()=>{dragging=false;pointerId=null;canvas.style.cursor='grab';requestFrame();};
  canvas.addEventListener('pointerup',endDrag);canvas.addEventListener('pointercancel',endDrag);canvas.addEventListener('lostpointercapture',endDrag);
  const resizeObserver=new ResizeObserver(resize);resizeObserver.observe(canvas);
  const intersectionObserver=new IntersectionObserver(entries=>{
    visible=entries[0].isIntersecting;
    if(visible)requestFrame();else if(raf){cancelAnimationFrame(raf);raf=0;previousTime=0;}
  },{rootMargin:'80px'});intersectionObserver.observe(canvas);
  document.addEventListener('visibilitychange',()=>{
    if(document.hidden){if(raf)cancelAnimationFrame(raf);raf=0;previousTime=0;}else requestFrame();
  });
  motionPreference.addEventListener('change',event=>{reducedMotion=event.matches;if(reducedMotion)paused=true;requestFrame();});
  window.sensorScene={
    setMode(next){if(!['both','water','soil'].includes(next))return;mode=next;resize();},
    setPaused(value){paused=Boolean(value);requestFrame();},
    reset(){targetYaw=-.38;targetPitch=.55;mode='both';time=0;resize();}
  };
  resize();window.dispatchEvent(new CustomEvent('scene-ready'));
})();
