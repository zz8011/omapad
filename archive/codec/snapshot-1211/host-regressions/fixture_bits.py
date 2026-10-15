import re

class Bits:
    def __init__(self,b): self.b=b; self.p=0; self.fields=[]
    def n(self,k):
        v=0
        for _ in range(k):
            v=(v<<1)|((self.b[self.p//8]>>(7-self.p%8))&1); self.p+=1
        return v
    def ue(self):
        z=0
        while self.n(1)==0: z+=1
        return (1<<z)-1+self.n(z)
    def se(self):
        v=self.ue(); return (v+1)//2 if v&1 else -v//2
    def f(self,name,kind):
        start=self.p; value=self.n(kind) if isinstance(kind,int) else getattr(self,kind)()
        self.fields.append({'name':name,'start_bit_in_nal_rbsp':start,'end_bit':self.p,'value':value})
        return value

def parse(fixture):
    nals=[x for x in re.split(b'\x00\x00\x00?\x01',fixture) if x]
    assert [x[0]&31 for x in nals]==[7,8,5]
    s=Bits(nals[0].replace(b'\x00\x00\x03',b'\x00\x00'))
    s.f('nal_header',8); assert s.f('profile_idc',8)==66
    s.f('constraint_flags',8);s.f('level_idc',8);s.f('sps_id','ue')
    framebits=s.f('log2_max_frame_num_minus4','ue')+4
    assert s.f('pic_order_cnt_type','ue')==2
    s.f('max_num_ref_frames','ue');s.f('gaps_flag',1)
    s.f('width_mbs_minus1','ue');s.f('height_map_units_minus1','ue')
    frameonly=s.f('frame_mbs_only_flag',1)
    mbaff=s.f('mb_adaptive_frame_field_flag',1) if not frameonly else 0
    s.f('direct_8x8_inference_flag',1); assert s.f('crop_flag',1)==0
    p=Bits(nals[1]);p.f('nal_header',8);p.f('pps_id','ue');p.f('sps_id','ue')
    assert p.f('entropy_coding_mode_flag',1)==0
    p.f('bottom_field_pic_order_present',1);assert p.f('slice_groups_minus1','ue')==0
    p.f('refs_l0_minus1','ue');p.f('refs_l1_minus1','ue')
    weighted=p.f('weighted_pred_flag',1); bipred=p.f('weighted_bipred_idc',2)
    initial=p.f('pic_init_qp_minus26','se');p.f('pic_init_qs_minus26','se');p.f('chroma_qp_offset','se')
    dbpresent=p.f('deblocking_control_present',1);intra=p.f('constrained_intra',1)
    assert p.f('redundant_pic_cnt_present',1)==0
    t=Bits(nals[2]);t.f('nal_header',8);assert t.f('first_mb','ue')==0
    stype=t.f('slice_type','ue');t.f('pps_id','ue');t.f('frame_num',framebits)
    field=t.f('field_pic_flag',1) if not frameonly else 0
    assert field==0
    t.f('idr_pic_id','ue');t.f('no_output_of_prior_pics',1);t.f('long_term_reference',1)
    delta=t.f('slice_qp_delta','se');db=t.f('disable_deblocking_filter_idc','ue') if dbpresent else 0
    alpha=t.f('alpha_offset_div2','se') if dbpresent and db!=1 else 0
    beta=t.f('beta_offset_div2','se') if dbpresent and db!=1 else 0
    assert (initial,delta,db,alpha,beta,t.p)==(-3,-3,1,0,0,32)
    
    
    return s,p,t,frameonly,mbaff,field,initial,delta,db,alpha,beta,stype
