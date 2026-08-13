/* Generated assets are inserted by tools/bwry/generate_web_converter.py. */
(function (root, factory) {
  const api = factory();
  if (typeof module !== "undefined" && module.exports) module.exports = api;
  else root.Bwry09k = api;
})(typeof globalThis !== "undefined" ? globalThis : this, function () {
  "use strict";

  const VERSION = "09k-selective-vintage-hybrid-web-v1";
  const SCREEN_WIDTH = 400;
  const SCREEN_HEIGHT = 300;
  const XN = 0.95047, YN = 1.0, ZN = 1.08883;
  const LAB_EPS = (6 / 29) ** 3;
  const LAB_KAPPA = 3 * (6 / 29) ** 2;
  const YULE_N = 1.57;
  const BLUE_NOISE_B64 = "__BLUE_NOISE_RANKS_BASE64__";

  // Measured media-relative linear reflectance. Device order is fixed:
  // black=0, white=1, yellow=2, red=3.
  const INK_LINEAR = [
    [0.06165, 0.05159, 0.05911],
    [1.0, 1.0, 1.0],
    [1.53391, 0.81167, 0.01309],
    [0.57015, 0.07022, 0.07061],
  ];

  const clamp = (v, lo, hi) => Math.max(lo, Math.min(hi, v));
  const smoothstep = (v, lo, hi) => {
    const t = clamp((v - lo) / Math.max(hi - lo, 1e-12), 0, 1);
    return t * t * (3 - 2 * t);
  };
  const hueDistance = (a, b) => {
    const d = Math.abs(a - b) % 360;
    return Math.min(d, 360 - d);
  };
  const hueOf = (a, b) => (Math.atan2(b, a) * 180 / Math.PI + 360) % 360;

  function linearToXyz(r, g, b) {
    return [
      0.4124564 * r + 0.3575761 * g + 0.1804375 * b,
      0.2126729 * r + 0.7151522 * g + 0.0721750 * b,
      0.0193339 * r + 0.1191920 * g + 0.9503041 * b,
    ];
  }

  function srgbChannelToLinear(v) {
    v /= 255;
    return v <= 0.04045 ? v / 12.92 : ((v + 0.055) / 1.055) ** 2.4;
  }

  function xyzToLab(x, y, z) {
    const f = (v) => v > LAB_EPS ? Math.cbrt(Math.max(v, 0)) : v / LAB_KAPPA + 4 / 29;
    const fx = f(x / XN), fy = f(y / YN), fz = f(z / ZN);
    return [116 * fy - 16, 500 * (fx - fy), 200 * (fy - fz)];
  }

  function labToXyz(l, a, b) {
    const fy = (l + 16) / 116;
    const fx = fy + a / 500;
    const fz = fy - b / 200;
    const inv = (v) => v > 6 / 29 ? v ** 3 : (v - 4 / 29) * LAB_KAPPA;
    return [inv(fx) * XN, inv(fy) * YN, inv(fz) * ZN];
  }

  function yuleEncode(x, y, z) {
    if (y <= 1e-12) return [0, 0, 0];
    const scale = Math.max(y, 0) ** (1 / YULE_N) / y;
    return [x * scale, y * scale, z * scale];
  }

  function yuleDecode(x, y, z) {
    const scale = Math.max(y, 0) ** (YULE_N - 1);
    return [x * scale, y * scale, z * scale];
  }

  function inverse3(m) {
    const a=m[0],b=m[1],c=m[2],d=m[3],e=m[4],f=m[5],g=m[6],h=m[7],i=m[8];
    const A=e*i-f*h, B=-(d*i-f*g), C=d*h-e*g;
    const det=a*A+b*B+c*C;
    return [A/det, (c*h-b*i)/det, (b*f-c*e)/det,
            B/det, (a*i-c*g)/det, (c*d-a*f)/det,
            C/det, (b*g-a*h)/det, (a*e-b*d)/det];
  }

  const PAL_XYZ = INK_LINEAR.map((v) => linearToXyz(v[0], v[1], v[2]));
  const PAL_LAB = PAL_XYZ.map((v) => xyzToLab(v[0], v[1], v[2]));
  const PAL_WORK = PAL_XYZ.map((v) => yuleEncode(v[0], v[1], v[2]));
  const V0 = PAL_WORK[0];
  const HULL_INV = inverse3([
    PAL_WORK[1][0]-V0[0], PAL_WORK[2][0]-V0[0], PAL_WORK[3][0]-V0[0],
    PAL_WORK[1][1]-V0[1], PAL_WORK[2][1]-V0[1], PAL_WORK[3][1]-V0[1],
    PAL_WORK[1][2]-V0[2], PAL_WORK[2][2]-V0[2], PAL_WORK[3][2]-V0[2],
  ]);
  const RED_H = hueOf(PAL_LAB[3][1], PAL_LAB[3][2]);
  const YELLOW_H = hueOf(PAL_LAB[2][1], PAL_LAB[2][2]);
  const MAX_CHROMA = Math.max(
    Math.hypot(PAL_LAB[2][1], PAL_LAB[2][2]),
    Math.hypot(PAL_LAB[3][1], PAL_LAB[3][2])
  );
  const PANEL_BLACK_L = PAL_LAB[0][0], PANEL_WHITE_L = PAL_LAB[1][0];

  let decodedMask = null;
  function decodeBase64(base64) {
    if (typeof Buffer !== "undefined") return Uint8Array.from(Buffer.from(base64, "base64"));
    const raw = atob(base64), out = new Uint8Array(raw.length);
    for (let i = 0; i < raw.length; i++) out[i] = raw.charCodeAt(i);
    return out;
  }

  function blueNoiseMask() {
    if (decodedMask) return decodedMask;
    const maskBytes = decodeBase64(BLUE_NOISE_B64);
    const mask = new Uint16Array(64 * 64);
    for (let i = 0; i < mask.length; i++) mask[i] = maskBytes[2*i] | (maskBytes[2*i+1] << 8);
    decodedMask = mask;
    return mask;
  }

  function buildGamutCandidates() {
    const count=1330;
    const mixX=new Float64Array(count),mixY=new Float64Array(count),mixZ=new Float64Array(count);
    const lightness=new Float64Array(count),chroma=new Float64Array(count),cosHue=new Float64Array(count),sinHue=new Float64Array(count);
    let p=0;
    for(let ia=0;ia<=18;ia++)for(let ib=0;ib<=18-ia;ib++)for(let ic=0;ic<=18-ia-ib;ic++){
      const id=18-ia-ib-ic,w0=ia/18,w1=ib/18,w2=ic/18,w3=id/18;
      const x=w0*PAL_WORK[0][0]+w1*PAL_WORK[1][0]+w2*PAL_WORK[2][0]+w3*PAL_WORK[3][0];
      const y=w0*PAL_WORK[0][1]+w1*PAL_WORK[1][1]+w2*PAL_WORK[2][1]+w3*PAL_WORK[3][1];
      const z=w0*PAL_WORK[0][2]+w1*PAL_WORK[1][2]+w2*PAL_WORK[2][2]+w3*PAL_WORK[3][2];
      const xyz=yuleDecode(x,y,z),lab=xyzToLab(xyz[0],xyz[1],xyz[2]),c=Math.hypot(lab[1],lab[2]),h=hueOf(lab[1],lab[2])*Math.PI/180;
      mixX[p]=x;mixY[p]=y;mixZ[p]=z;lightness[p]=lab[0];chroma[p]=c;cosHue[p]=Math.cos(h);sinHue[p]=Math.sin(h);p++;
    }
    if(p!==count)throw new Error(`unexpected gamut candidate count ${p}`);
    return {count,mixX,mixY,mixZ,lightness,chroma,cosHue,sinHue};
  }
  const GAMUT_CANDIDATES=buildGamutCandidates();

  function hullContains(x, y, z) {
    const v = yuleEncode(x, y, z);
    const dx=v[0]-V0[0], dy=v[1]-V0[1], dz=v[2]-V0[2];
    const w1=HULL_INV[0]*dx+HULL_INV[1]*dy+HULL_INV[2]*dz;
    const w2=HULL_INV[3]*dx+HULL_INV[4]*dy+HULL_INV[5]*dz;
    const w3=HULL_INV[6]*dx+HULL_INV[7]*dy+HULL_INV[8]*dz;
    const w0=1-w1-w2-w3;
    return w0 >= -1e-5 && w1 >= -1e-5 && w2 >= -1e-5 && w3 >= -1e-5;
  }

  function neutralLabAt(l) {
    const fy=(l+16)/116;
    const targetY=fy>6/29 ? fy**3 : (fy-4/29)*LAB_KAPPA;
    const targetMixY=Math.max(targetY,0)**(1/YULE_N);
    const t=clamp((targetMixY-PAL_WORK[0][1])/(PAL_WORK[1][1]-PAL_WORK[0][1]),0,1);
    const mx=PAL_WORK[0][0]+t*(PAL_WORK[1][0]-PAL_WORK[0][0]);
    const my=PAL_WORK[0][1]+t*(PAL_WORK[1][1]-PAL_WORK[0][1]);
    const mz=PAL_WORK[0][2]+t*(PAL_WORK[1][2]-PAL_WORK[0][2]);
    const xyz=yuleDecode(mx,my,mz);
    return xyzToLab(xyz[0],xyz[1],xyz[2]);
  }

  // Exact hard, constant-L hue-preserving map used only by 09k's adaptive
  // controller to measure how much visible colour the physical gamut loses.
  function faithfulMap(l, a, b) {
    const c=Math.hypot(a,b);
    const anchor=neutralLabAt(l);
    if (c < 1e-6) return anchor;
    const dl=l-anchor[0], da=a-anchor[1], db=b-anchor[2];
    let lo=0, hi=4;
    for (let n=0;n<18;n++) {
      const mid=(lo+hi)*0.5;
      const xyz=labToXyz(anchor[0]+mid*dl,anchor[1]+mid*da,anchor[2]+mid*db);
      if (hullContains(xyz[0],xyz[1],xyz[2])) lo=mid; else hi=mid;
    }
    const t=Math.min(1,Math.max(lo,1e-6));
    return [anchor[0]+t*dl,anchor[1]+t*da,anchor[2]+t*db];
  }

  function cuspLightness(h) {
    const wy=Math.exp(-0.5*(hueDistance(h,YELLOW_H)/55)**2);
    const wr=Math.exp(-0.5*(hueDistance(h,RED_H)/55)**2);
    return (wy*PAL_LAB[2][0]+wr*PAL_LAB[3][0])/Math.max(wy+wr,1e-12);
  }

  function compressFinalBase(l,a,b) {
    const c=Math.hypot(a,b),h=hueOf(a,b),cw=smoothstep(c,0,0.25*MAX_CHROMA);
    const anchor=neutralLabAt(l+0.18*cw*(cuspLightness(h)-l));
    if(c<1e-6)return anchor;
    const dl=l-anchor[0],da=a-anchor[1],db=b-anchor[2];
    let lo=0,hi=4;
    for(let n=0;n<18;n++){
      const mid=(lo+hi)*0.5,xyz=labToXyz(anchor[0]+mid*dl,anchor[1]+mid*da,anchor[2]+mid*db);
      if(hullContains(xyz[0],xyz[1],xyz[2]))lo=mid;else hi=mid;
    }
    const boundary=Math.max(lo,1e-6),kneeEff=1-0.2*cw,k=kneeEff*boundary,span=Math.max(boundary-k,1e-9);
    const compressed=k+span*(1-Math.exp(-Math.max(1-k,0)/span));
    const t=clamp(k>=1?1:compressed,0,1);
    return [anchor[0]+t*dl,anchor[1]+t*da,anchor[2]+t*db];
  }

  function mapSelectiveVivid(l,a,b) {
    const base=compressFinalBase(l,a,b),sourceC=Math.hypot(a,b),baseC=Math.hypot(base[1],base[2]);
    const recovery=0.72*smoothstep(sourceC,5,22)*(1-smoothstep(baseC,4,12));
    const vividBlend=smoothstep(sourceC,4,22);
    const alpha=recovery*vividBlend;
    if(alpha<=1e-6)return base;

    const targetC=Math.min(sourceC,MAX_CHROMA),targetH=hueOf(a,b)*Math.PI/180,cosTarget=Math.cos(targetH),sinTarget=Math.sin(targetH);
    const c=GAMUT_CANDIDATES;let best=0,bestScore=Infinity;
    for(let k=0;k<c.count;k++){
      const dl=l-c.lightness[k],dc=targetC-c.chroma[k];
      const cosDelta=cosTarget*c.cosHue[k]+sinTarget*c.sinHue[k];
      const hueTerm=2*targetC*c.chroma[k]*(1-clamp(cosDelta,-1,1));
      const score=4*dl*dl+0.65*dc*dc+0.10*hueTerm;
      if(score<bestScore){bestScore=score;best=k;}
    }
    const xyz=labToXyz(base[0],base[1],base[2]),baseMix=yuleEncode(xyz[0],xyz[1],xyz[2]);
    const mx=baseMix[0]+alpha*(c.mixX[best]-baseMix[0]);
    const my=baseMix[1]+alpha*(c.mixY[best]-baseMix[1]);
    const mz=baseMix[2]+alpha*(c.mixZ[best]-baseMix[2]);
    const out=yuleDecode(mx,my,mz);
    return xyzToLab(out[0],out[1],out[2]);
  }

  function percentile(values, pct) {
    const sorted=Array.from(values).sort((a,b)=>a-b);
    const pos=(sorted.length-1)*pct/100;
    const lo=Math.floor(pos), hi=Math.ceil(pos), f=pos-lo;
    return sorted[lo]*(1-f)+sorted[hi]*f;
  }

  function gaussianBlur(src,w,h,sigma) {
    if (sigma <= 0) return Float64Array.from(src);
    // scipy.ndimage.gaussian_filter uses radius=int(truncate*sigma+0.5).
    const radius=Math.floor(4*sigma+0.5), kernel=new Float64Array(2*radius+1);
    let sum=0;
    for(let k=-radius;k<=radius;k++){const v=Math.exp(-(k*k)/(2*sigma*sigma));kernel[k+radius]=v;sum+=v;}
    for(let k=0;k<kernel.length;k++)kernel[k]/=sum;
    const tmp=new Float64Array(src.length), out=new Float64Array(src.length);
    for(let y=0;y<h;y++)for(let x=0;x<w;x++){
      let v=0;for(let k=-radius;k<=radius;k++)v+=src[y*w+clamp(x+k,0,w-1)]*kernel[k+radius];tmp[y*w+x]=v;
    }
    for(let y=0;y<h;y++)for(let x=0;x<w;x++){
      let v=0;for(let k=-radius;k<=radius;k++)v+=tmp[clamp(y+k,0,h-1)*w+x]*kernel[k+radius];out[y*w+x]=v;
    }
    return out;
  }

  function buildToneLut(blackL,whiteL) {
    const lut=new Float64Array(1024), pivot=0.45;
    const g=Math.log(0.5)/Math.log(pivot), k=8*0.22;
    const sig=(t)=>1/(1+Math.exp(-k*(t-0.5))), slo=sig(0), shi=sig(1);
    const shadowAmount=0.14, highlightAmount=0.28, knee=1-0.55*highlightAmount;
    const span=1-knee, top=knee+span*(1-Math.exp(-(1-knee)/span));
    for(let i=0;i<lut.length;i++){
      const input=i/(lut.length-1)*100;
      let x=whiteL-blackL>1?(input-blackL)/(whiteL-blackL):input/100;x=clamp(x,0,1);
      const u=x**g, v=clamp((sig(u)-slo)/(shi-slo),0,1);x=v**(1/g);
      const lifted=x**(1/(1+2.2*shadowAmount));
      const sw=clamp(1-x/0.55,0,1)**1.5;x=x*(1-sw)+lifted*sw;
      if(x>knee){const over=x-knee;x=(knee+span*(1-Math.exp(-over/span)))/top;}
      lut[i]=x*100;
    }
    for(let i=1;i<lut.length;i++)if(lut[i]<lut[i-1])lut[i]=lut[i-1];
    return lut;
  }

  function applyTone(l,a,b,w,h) {
    let lo=percentile(l,0.5), hi=percentile(l,99.5);
    if(hi-lo<5){lo=0;hi=100;}
    const nearWhite=percentile(l,99.9);if(nearWhite>=96)hi=Math.max(hi,nearWhite);
    const lut=buildToneLut(lo,hi), outL=new Float64Array(l.length);
    for(let i=0;i<l.length;i++){
      const p=clamp(l[i],0,100)/100*(lut.length-1), p0=Math.floor(p), p1=Math.min(p0+1,lut.length-1), f=p-p0;
      outL[i]=lut[p0]*(1-f)+lut[p1]*f;
    }
    let blur=gaussianBlur(outL,w,h,6.0);
    for(let i=0;i<outL.length;i++)outL[i]=outL[i]+0.32*(outL[i]-blur[i]);
    blur=gaussianBlur(outL,w,h,1.6);
    for(let i=0;i<outL.length;i++)outL[i]=clamp(outL[i]+0.18*(outL[i]-blur[i]),0,100);
    const outA=new Float64Array(a.length),outB=new Float64Array(b.length);
    for(let i=0;i<a.length;i++){outA[i]=a[i]*1.18;outB[i]=b[i]*1.18;}
    return [outL,outA,outB];
  }

  function edgeStrength(l,w,h) {
    const src=gaussianBlur(l,w,h,0.6), mag=new Float64Array(l.length);
    const at=(x,y)=>src[clamp(y,0,h-1)*w+clamp(x,0,w-1)];
    for(let y=0;y<h;y++)for(let x=0;x<w;x++){
      const gx=(at(x+1,y-1)+2*at(x+1,y)+at(x+1,y+1))-(at(x-1,y-1)+2*at(x-1,y)+at(x-1,y+1));
      const gy=(at(x-1,y+1)+2*at(x,y+1)+at(x+1,y+1))-(at(x-1,y-1)+2*at(x,y-1)+at(x+1,y-1));
      mag[y*w+x]=Math.hypot(gx,gy);
    }
    const lo=percentile(mag,75),hi=percentile(mag,97),out=new Float64Array(l.length);
    if(hi-lo<1e-6)return out;
    for(let i=0;i<out.length;i++)out[i]=smoothstep(mag[i],lo,hi);
    return out;
  }

  function packCodes(codes) {
    const out=new Uint8Array(codes.length/4);
    for(let p=0;p<codes.length;p++)out[p>>2]|=codes[p]<<(6-((p&3)*2));
    return out;
  }

  function dither(l,a,b,gate,edge,w,h) {
    const n=l.length,tx=new Float64Array(n),ty=new Float64Array(n),tz=new Float64Array(n);
    for(let i=0;i<n;i++){
      const xyz=labToXyz(l[i],a[i],b[i]), work=yuleEncode(xyz[0],xyz[1],xyz[2]);tx[i]=work[0];ty[i]=work[1];tz[i]=work[2];
    }
    const ex=new Float64Array(n),ey=new Float64Array(n),ez=new Float64Array(n),codes=new Uint8Array(n);
    const mask=blueNoiseMask();
    const kernel=[[1,0,4/16],[2,0,3/16],[-2,1,1/16],[-1,1,2/16],[0,1,3/16],[1,1,2/16],[2,1,1/16]];
    for(let y=0;y<h;y++){
      const reverse=(y&1)===1;
      for(let step=0;step<w;step++){
        const x=reverse?w-1-step:step,i=y*w+x;
        const X=tx[i]+ex[i],Y=ty[i]+ey[i],Z=tz[i]+ez[i];
        const gain=Math.max(Y,0)**(YULE_N-1), lab=xyzToLab(X*gain,Y*gain,Z*gain);
        const tone=clamp((l[i]-PANEL_BLACK_L)/(PANEL_WHITE_L-PANEL_BLACK_L),0,1);
        const noise=((mask[(y&63)*64+(x&63)]+0.5)/4096-0.5)*5*(4*tone*(1-tone));
        const penalty=26*(1-gate[i]);
        let best=0,bestD=Infinity;
        for(let k=0;k<4;k++){
          const dl=lab[0]+noise-PAL_LAB[k][0],da=lab[1]-PAL_LAB[k][1],db=lab[2]-PAL_LAB[k][2];
          let dist=Math.hypot(dl,da,db);if(k>=2)dist+=penalty;
          if(dist<bestD){bestD=dist;best=k;}
        }
        codes[i]=best;
        const strength=1-0.45*edge[i];
        let rx=(X-PAL_WORK[best][0])*strength,ry=(Y-PAL_WORK[best][1])*strength,rz=(Z-PAL_WORK[best][2])*strength;
        const cs=gate[i],mx=ry*XN,mz=ry*ZN;rx=mx+(rx-mx)*cs;rz=mz+(rz-mz)*cs;
        for(const [dx,dy,weight] of kernel){
          const nx=reverse?x-dx:x+dx,ny=y+dy;if(nx<0||nx>=w||ny>=h)continue;
          const j=ny*w+nx;ex[j]+=rx*weight;ey[j]+=ry*weight;ez[j]+=rz*weight;
        }
      }
    }
    return codes;
  }

  function rgbaToBwryCodes(rgba,width=SCREEN_WIDTH,height=SCREEN_HEIGHT,options={}) {
    if(!rgba||rgba.length!==width*height*4)throw new Error(`rgba length must be ${width*height*4}`);
    const progress=typeof options.onProgress==="function"?options.onProgress:()=>{};
    const n=width*height,l=new Float64Array(n),a=new Float64Array(n),b=new Float64Array(n);
    progress("decode",0.05);
    for(let p=0,i=0;p<n;p++,i+=4){
      const xyz=linearToXyz(srgbChannelToLinear(rgba[i]),srgbChannelToLinear(rgba[i+1]),srgbChannelToLinear(rgba[i+2]));
      const lab=xyzToLab(xyz[0],xyz[1],xyz[2]);l[p]=lab[0];a[p]=lab[1];b[p]=lab[2];
    }
    progress("tone",0.15);
    const toned=applyTone(l,a,b,width,height),tl=toned[0],ta=toned[1],tb=toned[2];
    const need=new Float64Array(n),native=new Float64Array(n);let severity=0;
    progress("adaptive",0.35);
    for(let i=0;i<n;i++){
      const sl=PANEL_BLACK_L+tl[i]*(PANEL_WHITE_L-PANEL_BLACK_L)/100,sa=ta[i],sb=tb[i];
      const faithful=faithfulMap(sl,sa,sb),sc=Math.hypot(sa,sb),fc=Math.hypot(faithful[1],faithful[2]);
      const cw=smoothstep(sc,5,22),visible=smoothstep(fc,4,11);
      const he=hueDistance(hueOf(sa,sb),hueOf(faithful[1],faithful[2]));
      native[i]=cw*visible*(1-smoothstep(he,10,38));
      need[i]=clamp(cw*((1-visible)+0.58*visible*smoothstep(he,18,75)),0,1);severity+=need[i];
    }
    severity/=n;
    const imageFactor=0.55+0.45*smoothstep(severity,0.06,0.42);
    const ml=new Float64Array(n),ma=new Float64Array(n),mb=new Float64Array(n),gate=new Float64Array(n);
    let redH=RED_H,yellowH=YELLOW_H,warmSpan=(yellowH-redH+360)%360;
    if(warmSpan>180){const t=redH;redH=yellowH;yellowH=t;warmSpan=(yellowH-redH+360)%360;}
    progress("gamut",0.55);
    for(let i=0;i<n;i++){
      const c=Math.hypot(ta[i],tb[i]),h=hueOf(ta[i],tb[i]);
      const redAffinity=Math.exp(-0.5*(hueDistance(h,redH)/68)**2),yellowAffinity=Math.exp(-0.5*(hueDistance(h,yellowH)/68)**2);
      const colourful=smoothstep(c,4,28),yellowMix=0.67+colourful*(yellowAffinity/Math.max(redAffinity+yellowAffinity,1e-12)-0.67);
      const targetH=(redH+yellowMix*warmSpan)%360,x=clamp(tl[i]/100,0,1);
      const targetC=Math.max(Math.sin(Math.PI*x),0)**0.72*(2.5+colourful*(34-2.5));
      const amount=clamp(0.84*imageFactor*(0.04+0.96*need[i])*(1-0.78*native[i]),0,1);
      const deltaH=(targetH-h+540)%360-180,outH=(h+amount*deltaH+360)%360,outC=Math.max(c+amount*(targetC-c),0),rad=outH*Math.PI/180;
      const rangedL=PANEL_BLACK_L+tl[i]*(PANEL_WHITE_L-PANEL_BLACK_L)/100;
      const mapped=mapSelectiveVivid(rangedL,outC*Math.cos(rad),outC*Math.sin(rad));
      const open=smoothstep(Math.hypot(mapped[1],mapped[2]),3.5,13);gate[i]=open;
      ml[i]=mapped[0];ma[i]=mapped[1]*open;mb[i]=mapped[2]*open;
    }
    progress("edges",0.75);
    const edge=edgeStrength(ml,width,height);
    progress("dither",0.82);
    const codes=dither(ml,ma,mb,gate,edge,width,height);
    progress("done",1);
    return codes;
  }

  function rgbaToBwry2bpp(rgba,width=SCREEN_WIDTH,height=SCREEN_HEIGHT,options={}) {
    return packCodes(rgbaToBwryCodes(rgba,width,height,options));
  }

  return {VERSION,SCREEN_WIDTH,SCREEN_HEIGHT,rgbaToBwryCodes,rgbaToBwry2bpp,packCodes};
});
